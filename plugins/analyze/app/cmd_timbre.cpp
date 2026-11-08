// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze timbre <reference.wav> <target.wav> [--bands N] [--out report.json]
//
// Reduces both files to their long-term third-octave curve and writes the
// difference and the EQ bands that approximate it. The reference is read
// and closed; only its curve goes in the report. Exit: 0 report written,
// 1 failed, 2 usage, 130 cancelled (ADR-0046).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "commands.h"
#include "sonora/json/json.h"
#include "sonora/wav_reader.h"
#include "timbre_fit.h"

namespace {

using sonora::json::Value;
using namespace sonora::analyze;

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
    sonora::WavReader reader_;
};

Value num(double v) { return Value::number_float(static_cast<float>(v)); }

int usage() {
    std::fputs("usage: sonora-analyze timbre <reference.wav> <target.wav> [--bands N] [--out report.json]\n", stderr);
    return 2;
}

int fail(const std::string& what) {
    std::fprintf(stderr, "sonora-analyze: %s\n", what.c_str());
    return 1;
}

bool measure_file(const std::string& path, const RunOptions& run, TimbreCurve& curve, std::string& error) {
    WavSource src;
    if (!src.open(path, error)) {
        error = path + ": " + error;
        return false;
    }
    WelchTimbreMeasurer measurer;
    return measurer.measure(src, run, curve, error);
}

Value curve_json(const TimbreCurve& c) {
    Value a = Value::array();
    for (int b = 0; b < kTimbreBands; ++b) a.push(c.valid[b] ? num(c.gain_db[b]) : Value::null());
    return a;
}

const char* kind_name(EqBandKind k) {
    switch (k) {
        case EqBandKind::LowShelf: return "low_shelf";
        case EqBandKind::HighShelf: return "high_shelf";
        default: return "bell";
    }
}

Value report_json(const TimbreCurve& ref, const TimbreCurve& tgt, const TimbreFit& fit) {
    Value report = Value::object();
    Value centres = Value::array();
    for (double f : kThirdOctaveCentreHz) centres.push(num(f));
    report.set("band_centres_hz", std::move(centres));
    report.set("reference_db", curve_json(ref));
    report.set("target_db", curve_json(tgt));
    Value diff = Value::array();
    for (int b = 0; b < kTimbreBands; ++b) diff.push(fit.valid[b] ? num(fit.result.curve_db[b]) : Value::null());
    report.set("correction_db", std::move(diff));
    Value bands = Value::array();
    for (std::size_t i = 0; i < fit.result.eq_bands.size(); ++i) {
        const EqBand& e = fit.result.eq_bands[i];
        Value o = Value::object();
        o.set("type", Value::string(kind_name(fit.kinds[i])));
        o.set("freq_hz", num(e.freq_hz));
        o.set("gain_db", num(e.gain_db));
        o.set("q", num(e.q));
        bands.push(std::move(o));
    }
    report.set("eq_bands", std::move(bands));
    report.set("curve_rms_db", num(fit.curve_rms_db));
    report.set("residual_rms_db", num(fit.residual_rms_db));
    return report;
}

}  // namespace

namespace sonora::analyze::app {

int run_timbre_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    if (args.size() < 2) return usage();
    TimbreMatchParams params;
    params.max_bands = kTimbreMaxFitBands;
    std::string out_path;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (i + 1 >= args.size()) return usage();
        if (args[i] == "--bands") {
            params.max_bands = std::atoi(args[++i].c_str());
        } else if (args[i] == "--out") {
            out_path = args[++i];
        } else {
            return usage();
        }
    }
    std::string error;
    if (!timbre_params_valid(params) || params.max_bands > kTimbreMaxFitBands) {
        std::fprintf(stderr, "sonora-analyze: --bands must be 1 to %d\n", kTimbreMaxFitBands);
        return 2;
    }

    RunOptions run;
    run.cancel = &cancel;
    TimbreCurve ref, tgt;
    if (!measure_file(args[0], run, ref, error) || !measure_file(args[1], run, tgt, error))
        return error == "cancelled" ? 130 : fail(error);
    TimbreFit fit;
    if (!match_timbre_detailed(ref, tgt, params, fit, error)) return fail(error);

    std::string text;
    if (!sonora::json::write(report_json(ref, tgt, fit), text, error)) return fail(error);
    if (out_path.empty()) {
        std::cout << text;
        return 0;
    }
    std::ofstream f(out_path, std::ios::binary);
    f << text;
    return f ? 0 : fail("cannot write " + out_path);
}

}  // namespace sonora::analyze::app
