// SPDX-License-Identifier: GPL-3.0-or-later
//
// The imager against its bars: magnitude flat at 100 % in every band (the
// Linkwitz-Riley sum), each leg 6 dB down at its crossover, the width of
// each band set where the signal sits well inside it, a zipper sweep of the
// widths on a side-only tone (the common sweep's tone is centred, so a
// width would be inaudible in it), and the common checks.
#include <cmath>
#include <string>

#include "sonora/measure/measure.h"
#include "sonora/native/imager.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

Chans run(Suite& s, const Config& c, const Chans& x, double fs) {
    auto p = s.make();
    measure::apply(*p, c);
    p->prepare(fs, 128, static_cast<std::uint32_t>(x.size()));
    return measure::render(*p, x);
}

// A tone with L = -R: pure side.
Chans side_tone(double fs, double f, double dbfs, double seconds) {
    Chans x = measure::sine(fs, f, dbfs, seconds);
    for (auto& v : x[1]) v = -v;
    return x;
}

double gain_db(const Chans& y, const Chans& x) {
    const std::size_t from = x[0].size() / 2;
    return measure::to_db(measure::rms(y[0], from) / measure::rms(x[0], from));
}

std::string rate_name(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

void flat(Suite& s) {
    for (double fs : measure::kRates) {
        double worst = 0.0;
        for (double f : {40.0, 100.0, 150.0, 300.0, 1000.0, 3000.0, 4000.0, 6000.0, 12000.0}) {
            // Centred and pure-side tones both: width 100 % is identity in magnitude.
            const Chans c = measure::sine(fs, f, -12.0, 1.0);
            worst = std::fmax(worst, std::fabs(gain_db(run(s, {}, c, fs), c)));
            const Chans sd = side_tone(fs, f, -12.0, 1.0);
            worst = std::fmax(worst, std::fabs(gain_db(run(s, {}, sd, fs), sd)));
        }
        s.check_le("100 % in every band: magnitude flat 40 Hz to 12 kHz, " + rate_name(fs) + ", |gain|", worst,
                   s.bar("flat_db"), "dB");
    }
}

void crossovers(Suite& s) {
    for (double fs : measure::kRates) {
        // Low band removed from a side-only tone at the low crossover: what
        // remains is the high leg, 6.02 dB down (Linkwitz-Riley).
        const Chans sd = side_tone(fs, 150.0, -12.0, 1.0);
        const double g = gain_db(run(s, {{"width_low", 0.0f}}, sd, fs), sd);
        s.check_le("low crossover: remaining leg at 150 Hz is -6.02 dB, " + rate_name(fs) + ", |error|",
                   std::fabs(g + 6.0206), s.bar("crossover_db"), "dB");
        // The same at the upper crossover.
        const Chans sh = side_tone(fs, 4000.0, -12.0, 1.0);
        const double h = gain_db(run(s, {{"width_high", 0.0f}}, sh, fs), sh);
        s.check_le("high crossover: remaining leg at 4 kHz is -6.02 dB, " + rate_name(fs) + ", |error|",
                   std::fabs(h + 6.0206), s.bar("crossover_db"), "dB");
    }
}

void widths(Suite& s) {
    const double fs = 48000.0;
    {
        const Chans sd = side_tone(fs, 50.0, -12.0, 1.0);
        const Chans y = run(s, {{"low_xover", 500.0f}, {"width_low", 0.0f}}, sd, fs);
        s.check_le("low width 0 %: 50 Hz side with the crossover at 500 Hz, gain", gain_db(y, sd), s.bar("narrow_db"), "dB");
    }
    {
        const Chans sd = side_tone(fs, 1000.0, -12.0, 1.0);
        const Chans y = run(s, {{"low_xover", 40.0f}, {"high_xover", 12000.0f}, {"width_mid", 0.0f}}, sd, fs);
        s.check_le("mid width 0 %: 1 kHz side, gain", gain_db(y, sd), s.bar("narrow_db"), "dB");
    }
    {
        const Chans sd = side_tone(fs, 10000.0, -12.0, 1.0);
        const Chans y = run(s, {{"high_xover", 1000.0f}, {"width_high", 200.0f}}, sd, fs);
        s.check_le("high width 200 %: 10 kHz side doubled, |gain - 6.02 dB|", std::fabs(gain_db(y, sd) - 6.0206),
                   s.bar("width_db"), "dB");
    }
    {
        // A centred signal is untouched whatever the widths: mono compatibility.
        const Chans c = measure::sine(fs, 700.0, -12.0, 1.0);
        const Chans y = run(s, {{"width_low", 0.0f}, {"width_mid", 0.0f}, {"width_high", 200.0f}}, c, fs);
        s.check_le("centred 700 Hz at widths 0/0/200: |gain|", std::fabs(gain_db(y, c)), s.bar("flat_db"), "dB");
    }
    {
        const Chans x = measure::sine(fs, 997.0, -12.0, 0.5, 1);
        const Chans y = run(s, {{"width_mid", 0.0f}}, x, fs);
        s.check_le("one channel: 997 Hz passes, |gain|", std::fabs(gain_db(y, x)), s.bar("flat_db"), "dB");
    }
}

// The width and crossover sweeps of the common zipper check, on a tone that
// has a side.
void zipper_side(Suite& s) {
    const double fs = 48000.0;
    Chans in = side_tone(fs, 100.0, -6.0, 3.25);
    const auto fade = static_cast<std::size_t>(0.01 * fs);
    for (auto& ch : in) {
        for (std::size_t i = 0; i < fade; ++i) ch[i] *= static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979 * i / fade));
    }
    for (const char* id : {"width_low", "width_mid", "width_high"}) {
        auto p = s.make();
        measure::apply(*p, {});
        p->prepare(fs, 128, 2);
        const std::uint32_t idx = measure::param_index(*p, id);
        const native::ParamInfo& info = p->descriptor().params[idx];
        std::vector<measure::Event> ev;
        const std::uint64_t start = static_cast<std::uint64_t>(0.25 * fs);
        for (std::uint64_t f = start; f < in[0].size(); f += 128) {
            const double t = static_cast<double>(f - start) / fs;
            // Down to the bottom, up to the top, one range per second.
            const double n = t < 0.5 ? 0.5 - t : (t < 1.5 ? t - 0.5 : 1.0 - (t - 1.5));
            ev.push_back({f, idx, static_cast<float>(native::from_normalized(info, std::fmin(1.0, std::fmax(0.0, n))))});
        }
        const Chans out = measure::render(*p, in, ev);
        double worst = 0.0;
        for (const auto& ch : out) worst = std::fmax(worst, measure::hf_residue_peak(ch, fs, start));
        s.check_le(std::string("zipper, side tone: full-range sweep of '") + id + "', HF residue peak",
                   measure::to_db(worst), s.bar("zipper_dbfs"), "dBFS");
    }
}

}  // namespace

measure::Config imager_golden_config() {
    return {{"low_xover", 200.0f}, {"high_xover", 3000.0f}, {"width_low", 50.0f}, {"width_mid", 120.0f},
            {"width_high", 160.0f}};
}

int imager(const measure::Args& args) {
    Suite s("imager", [] { return make_imager(); }, args);
    flat(s);
    crossovers(s);
    widths(s);
    zipper_side(s);
    // No check_latency configuration: the harness measures latency as the
    // cross-correlation lag, which for an all-pass filter bank is its phase
    // delay (1-2 samples), not buffering. The imager adds no buffering and
    // reports 0 (PDC has nothing to compensate); the figure is held here.
    for (double fs : measure::kRates) {
        auto p = s.make();
        p->prepare(fs, 128, 2);
        s.check_le("latency reported (no look-ahead, no buffering), " + rate_name(fs), static_cast<double>(p->latency()),
                   0.0, "samples");
    }
    measure::check_common(s, imager_golden_config(), {});
    return s.finish();
}

}  // namespace sonora::native::suites
