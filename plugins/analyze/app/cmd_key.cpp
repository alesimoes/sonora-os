// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze key <wav> [--out report.json] (ADR-0066 §4): the classical
// key pass. The report is written only after the pass finished, so a
// cancelled run (exit 130) leaves no file.
#include <cstdio>
#include <fstream>
#include <iostream>

#include "commands.h"
#include "key/classical_key_detector.h"
#include "sonora/json/json.h"
#include "sonora/wav_reader.h"

namespace sonora::analyze::app {

namespace {

class WavSource final : public AudioSource {
public:
    bool open(const std::string& path, std::string& error) { return reader_.open(path, error); }
    std::uint32_t channels() const override { return reader_.channels(); }
    double sample_rate() const override { return reader_.sample_rate(); }
    std::int64_t frames() const override { return reader_.frames(); }
    std::int64_t read(std::int64_t start, std::int64_t count, float* out) override {
        return reader_.read(start, count, out);
    }

private:
    WavReader reader_;
};

int key_usage() {
    std::fputs("usage: sonora-analyze key <wav> [--out report.json]\n", stderr);
    return 2;
}

}  // namespace

int run_key_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    if (args.empty()) return key_usage();
    std::string out_path;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] != "--out" || i + 1 >= args.size()) return key_usage();
        out_path = args[++i];
    }

    std::string error;
    WavSource src;
    if (!src.open(args[0], error)) {
        std::fprintf(stderr, "sonora-analyze: %s: %s\n", args[0].c_str(), error.c_str());
        return 1;
    }
    RunOptions run;
    run.cancel = &cancel;
    ClassicalKeyDetector detector;
    KeyResult r;
    if (!detector.detect(src, run, r, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return error == "cancelled" ? 130 : 1;
    }

    using json::Value;
    Value report = Value::object();
    report.set("pass", Value::string("key"));
    report.set("method", Value::string(key_method_name(r.method)));
    report.set("tonic", Value::integer(r.tonic));
    report.set("tonic_name", Value::string(tonic_name(r.tonic)));
    report.set("mode", Value::string(key_mode_name(r.mode)));
    report.set("confidence", Value::number_float(static_cast<float>(r.confidence)));
    report.set("duration_s", Value::number_float(static_cast<float>(static_cast<double>(src.frames()) / src.sample_rate())));
    std::string text;
    if (!json::write(report, text, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return 1;
    }
    if (out_path.empty()) {
        std::cout << text;
        return 0;
    }
    std::ofstream f(out_path, std::ios::binary);
    f << text;
    if (!f) {
        std::fprintf(stderr, "sonora-analyze: cannot write %s\n", out_path.c_str());
        return 1;
    }
    return 0;
}

}  // namespace sonora::analyze::app
