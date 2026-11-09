// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze beat <wav> [--out report.json] (ADR-0114): the classical
// beat pass. The report is written only after the pass finished, so a
// cancelled run (exit 130) leaves no file. Exit 0 done, 1 failed, 2 usage.
#include <cstdio>
#include <fstream>
#include <iostream>

#include "beat/classical_beat_detector.h"
#include "commands.h"
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

int beat_usage() {
    std::fputs("usage: sonora-analyze beat <wav> [--out report.json]\n", stderr);
    return 2;
}

}  // namespace

int run_beat_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    if (args.empty()) return beat_usage();
    std::string out_path;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] != "--out" || i + 1 >= args.size()) return beat_usage();
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
    ClassicalBeatDetector detector;
    BeatResult r;
    if (!detector.detect(src, run, r, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return error == "cancelled" ? 130 : 1;
    }
    std::string text;
    if (!beat_report_to_json(r, text, error)) {
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
