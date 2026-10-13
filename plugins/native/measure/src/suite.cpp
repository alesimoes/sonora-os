// SPDX-License-Identifier: GPL-3.0-or-later
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

int Suite::finish() {
    if (!args_.report_dir.empty()) {
        Value root = Value::object();
        root.set("processor", Value::string(name_));
        root.set("failures", Value::integer(failures_));
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
    std::printf("%s: %s (%d failing)\n", name_.c_str(), failures_ == 0 ? "meets its bars" : "FAILS", failures_);
    return failures_ == 0 ? 0 : 1;
}

}  // namespace sonora::measure
