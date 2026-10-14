// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "sonora/measure/measure.h"

namespace sonora::measure {

using json::Value;

Suite::Suite(std::string name, Factory factory, Args args)
    : name_(std::move(name)), factory_(std::move(factory)), args_(std::move(args)) {
    std::ifstream f(args_.quality_path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Error e;
    if (!f.good() && text.empty()) {
        fail("cannot read " + args_.quality_path);
    } else if (!json::parse(text, quality_, e)) {
        fail(args_.quality_path + ": " + e.message + " at line " + std::to_string(e.line));
    }
}

const Value* Suite::quality(const std::string& path) const {
    const Value* v = &quality_;
    std::size_t start = 0;
    while (v != nullptr && start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        v = v->is_object() ? v->find(key) : nullptr;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return v;
}

double Suite::bar(const std::string& path) {
    const Value* v = quality(path);
    double d = 0.0;
    if (v == nullptr || !v->to_double(d)) {
        fail("quality.json has no numeric bar '" + path + "'");
        return 0.0;
    }
    return d;
}

namespace {
Value num(double v) {
    // Non-finite measurements are recorded as null, not dropped.
    return std::isfinite(v) ? Value::number_float(static_cast<float>(v)) : Value::null();
}
}  // namespace

void Suite::check(const std::string& what, double value, double b, bool pass,
                  const std::string& unit, const std::string& note) {
    Value o = Value::object();
    o.set("check", Value::string(what));
    o.set("value", num(value));
    o.set("bar", num(b));
    o.set("unit", Value::string(unit));
    o.set("pass", Value::boolean(pass));
    if (!note.empty()) o.set("note", Value::string(note));
    checks_.push(std::move(o));
    std::printf("  %-4s %-58s %12.5g %-6s (bar %g)%s%s\n", pass ? "ok" : "FAIL", what.c_str(), value,
                unit.c_str(), b, note.empty() ? "" : "  ", note.c_str());
    if (!pass) ++failures_;
}

void Suite::info(const std::string& what, double value, const std::string& unit,
                 const std::string& note) {
    Value o = Value::object();
    o.set("check", Value::string(what));
    o.set("value", num(value));
    o.set("unit", Value::string(unit));
    o.set("report_only", Value::boolean(true));
    if (!note.empty()) o.set("note", Value::string(note));
    checks_.push(std::move(o));
    std::printf("  %-4s %-58s %12.5g %-6s%s%s\n", "info", what.c_str(), value, unit.c_str(),
                note.empty() ? "" : "  ", note.c_str());
}

void Suite::fail(const std::string& why) {
    std::printf("  FAIL %s\n", why.c_str());
    Value o = Value::object();
    o.set("check", Value::string(why));
    o.set("pass", Value::boolean(false));
    checks_.push(std::move(o));
    ++failures_;
}

void Suite::start_cpu_calibration() {
    if (kernel_before_us_ > 0.0) return;
    kernel_before_us_ = reference_kernel_us(std::min(20.0, args_.cpu_seconds));
}

void Suite::calibrated_cpu(const std::string& what, double p999_us, double max_us) {
    calibrated_.push_back({what, p999_us, max_us});
}

void Suite::finish_cpu_calibration() {
    if (calibrated_.empty()) return;
    const double before = kernel_before_us_;
    const double after = reference_kernel_us(std::min(20.0, args_.cpu_seconds));
    const double ref = reference_kernel_reference_us();
    const std::string how = "p50, per-block minimum of 5 runs";
    info("CPU reference kernel per block, this machine, before the loads", before, "us", how);
    info("CPU reference kernel per block, this machine, after the loads", after, "us", how);
    info("CPU reference kernel per block, reference machine", ref, "us", "calibrate.cpp");
    // The kernel's p50 holds to 0.1 % on a quiet machine. A larger change
    // between the two timings is the machine changing (a neighbour
    // arriving, a clock step) while the loads ran, which no one scale can
    // correct: the comparison would be a coin flip (review of PR #55).
    const double drift = std::fabs(after - before) / std::min(before, after);
    info("CPU reference kernel, after against before", 100.0 * drift, "%",
         "over 5 % the calibrated CPU checks are inconclusive");
    if (!(drift <= kCalibrationDrift)) {
        inconclusive_ = true;
        std::printf("  INCONCLUSIVE: the machine's speed changed during the run (reference kernel %.2f us before "
                    "the loads, %.2f us after); the calibrated CPU checks are neither passed nor failed\n",
                    before, after);
        for (const auto& c : calibrated_) {
            info(c.what + "p99.9, inconclusive", c.p999_us, "us", "not held to a bar");
            info(c.what + "max, inconclusive", c.max_us, "us", "not held to a bar");
        }
        return;
    }
    const double kernel = 0.5 * (before + after);
    const double scale = kernel / ref;
    info("CPU runner scale", scale, "", "the CPU class bars are multiplied by it");
    char note[160];
    std::snprintf(note, sizeof note,
                  "per-block minimum of 5 runs; class bar x %.3f, this machine's reference kernel against the "
                  "reference machine's",
                  scale);
    const bool baseline = quality("cpu.p999_kernels") != nullptr;
    for (const auto& c : calibrated_) {
        check_le(c.what + "p99.9, runner-calibrated", c.p999_us, bar("cpu.p999_us") * scale, "us", note);
        check_le(c.what + "max, runner-calibrated", c.max_us, bar("cpu.max_us") * scale, "us", note);
        // The scaled class bar alone trips on a regression of 20 % on one
        // runner and 28 % on another (review of PR #55). The recorded
        // worst case in kernels, the most any measured machine gave,
        // trips on 15 % on any of them.
        if (baseline) {
            check_le(c.what + "p99.9 in reference kernels", c.p999_us / kernel,
                     bar("cpu.p999_kernels") * kKernelRegression, "kernels",
                     "cpu.p999_kernels x 1.15: the recorded worst case, plus 15 %");
        }
    }
}

int Suite::finish() {
    finish_cpu_calibration();
    if (!args_.report_dir.empty()) {
        Value root = Value::object();
        root.set("processor", Value::string(name_));
        root.set("failures", Value::integer(failures_));
        root.set("inconclusive", Value::boolean(inconclusive_));
        root.set("checks", std::move(checks_));
        root.set("data", std::move(data_));
        std::string out, err;
        if (json::write(root, out, err)) {
            std::ofstream f(args_.report_dir + "/" + name_ + ".json", std::ios::binary | std::ios::trunc);
            f << out;
        }
        checks_ = Value::array();
        data_ = Value::object();
    }
    if (failures_ == 0 && inconclusive_) {
        std::printf("%s: INCONCLUSIVE (0 failing; the calibrated CPU checks could not be held, see above)\n",
                    name_.c_str());
        return kInconclusive;
    }
    std::printf("%s: %s (%d failing)\n", name_.c_str(), failures_ == 0 ? "meets its bars" : "FAILS", failures_);
    return failures_ == 0 ? 0 : 1;
}

}  // namespace sonora::measure
