// SPDX-License-Identifier: GPL-3.0-or-later
//
// The limiter against its bars: zero true-peak overs on a band-limited
// stress set measured by a 16x reference finer than the limiter's own
// detector, transparency below the ceiling, release from a step test,
// aliasing on steady tones, and the common checks.
#include <cmath>
#include <functional>
#include <string>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/limiter.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

struct Stress {
    const char* name;
    std::function<Chans(double)> make;
};

std::vector<Stress> stress() {
    return {
        {"noise to 20 kHz, +12 dB", [](double fs) {
             Chans x = measure::noise(fs, 12.0, 1.0, 99u);
             measure::band_limit_and_fade(x, fs);
             return x;
         }},
        {"noise to 20 kHz, +24 dB", [](double fs) {
             Chans x = measure::noise(fs, 24.0, 1.0, 5u);
             measure::band_limit_and_fade(x, fs);
             return x;
         }},
        {"fs/4 sine at 45 deg, +12 dB", [](double fs) {
             Chans x = measure::silence(fs, 1.0);
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 x[0][i] = x[1][i] = static_cast<float>(4.0 * std::sin(dsp::kTwoPi * i / 4.0 + dsp::kPi / 4.0));
             }
             measure::band_limit_and_fade(x, fs);
             return x;
         }},
        {"square 1 kHz to 20 kHz, +6 dB", [](double fs) {
             Chans x = measure::silence(fs, 1.0);
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 x[0][i] = x[1][i] = std::fmod(1000.0 * i / fs, 1.0) < 0.5 ? 2.0f : -2.0f;
             }
             measure::band_limit_and_fade(x, fs);
             return x;
         }},
        {"log sweep 20 Hz-20 kHz, +12 dB", [](double fs) {
             Chans x = measure::log_sweep(fs, 20.0, 20000.0, 12.0, 1.0, 2);
             measure::band_limit_and_fade(x, fs);
             return x;
         }},
    };
}

void ceiling(Suite& s) {
    double worst = -1e9;
    int overs = 0;
    for (double fs : measure::kRates) {
        for (double ceil_db : {-1.0, -0.1}) {
            for (const Stress& st : stress()) {
                auto p = s.make();
                measure::apply(*p, {{"ceiling", static_cast<float>(ceil_db)}, {"release", 20.0f}});
                p->prepare(fs, 128, 2);
                const Chans y = measure::render(*p, st.make(fs));
                double tp = 0.0;
                for (const auto& c : y) tp = std::fmax(tp, measure::reference_true_peak(c, dsp::db_to_gain(ceil_db - 6.0)));
                const double margin = dsp::gain_to_db(tp) - ceil_db;
                worst = std::fmax(worst, margin);
                if (margin > 0.0) ++overs;
            }
        }
    }
    s.check("true-peak overs above the ceiling (16x reference; 5 signals x 2 ceilings x 3 rates)",
            overs, 0.0, overs == 0, "count");
    s.info("highest output true peak relative to the ceiling", worst, "dB", "target is 0.05 dB under");
}

void transparency(Suite& s) {
    for (double fs : measure::kRates) {
        auto p = s.make();
        p->prepare(fs, 128, 2);
        Chans x = measure::noise(fs, -12.0, 0.5, 8u);
        measure::band_limit_and_fade(x, fs);
        const Chans y = measure::render(*p, x);
        const std::size_t L = p->latency();
        double worst = 0.0;
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t i = L; i < y[c].size(); ++i) worst = std::fmax(worst, std::fabs(y[c][i] - x[c][i - L]));
        }
        s.check("below the ceiling: output = input delayed by latency, " + std::to_string(static_cast<int>(fs)) + " Hz",
                worst, 0.0, worst == 0.0, "abs");
    }
}

void release(Suite& s) {
    const double tol = s.bar("limiter.time_tolerance");
    for (double fs : {44100.0, 48000.0}) {
        for (float r : {50.0f, 200.0f, 800.0f}) {
            auto p = s.make();
            measure::apply(*p, {{"ceiling", -1.0f}, {"release", r}, {"true_peak", 0.0f}});
            p->prepare(fs, 128, 2);
            // DC: -6 dBFS, then +6 dBFS for 0.5 s, then -6 dBFS again.
            Chans x = measure::silence(fs, 0.3 + 0.5 + 6.0 * r * 1e-3 + 0.3);
            const auto a = static_cast<std::size_t>(0.3 * fs), b = static_cast<std::size_t>(0.8 * fs);
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                x[0][i] = x[1][i] = static_cast<float>(i >= a && i < b ? 2.0 : 0.5);
            }
            const Chans y = measure::render(*p, x);
            const std::size_t L = p->latency();
            auto g = [&](std::size_t i) { return static_cast<double>(y[0][i + L]) / x[0][i]; };
            // Gain while limited, and the first sample it rises after b.
            const double g_low = g(b - 1);
            std::size_t start = b;
            while (start < x[0].size() - L && g(start) <= g_low * (1.0 + 1e-9)) ++start;
            std::size_t k = start;
            const double mark = g_low + (1.0 - std::exp(-1.0)) * (1.0 - g_low);
            while (k < x[0].size() - L && g(k) < mark) ++k;
            const double tr = (k - start + 1) / fs;
            s.check_le("release " + std::to_string(static_cast<int>(r)) + " ms, |measured/set - 1| at " +
                           std::to_string(static_cast<int>(fs)) + " Hz",
                       std::fabs(tr / (r * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(tr * 1e3) + " ms (63 % of the linear gain)");
        }
    }
}

void aliasing(Suite& s) {
    const double bar = s.bar("limiter.alias_dbc");
    const double fs = 48000.0;
    for (double f : {1000.0, 5000.0, 10000.0, 15000.0}) {
        auto p = s.make();
        measure::apply(*p, {{"ceiling", -1.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, f, 5.0, 1.5));
        s.check_le("non-harmonic energy, " + std::to_string(static_cast<int>(f)) + " Hz 6 dB over the ceiling",
                   measure::alias_dbc(y[0], 38400, fs, f), bar, "dBc");
    }
}

}  // namespace

int limiter(const measure::Args& args) {
    Suite s("limiter", [] { return make_limiter(); }, args);
    ceiling(s);
    transparency(s);
    release(s);
    aliasing(s);
    const Config golden = {{"input", 9.0f}, {"ceiling", -1.0f}, {"release", 40.0f}};
    // The tone at -6 dBFS through 12 dB of drive is limited throughout, so
    // every swept parameter changes the gain the tone gets.
    const Config zipper = {{"input", 12.0f}};
    measure::check_common(s, golden,
                          {{"look-ahead 1.5 ms", {}},
                           {"look-ahead 1 ms", {{"lookahead", 1.0f}}},
                           {"look-ahead 5 ms, sample peak", {{"lookahead", 5.0f}, {"true_peak", 0.0f}}}},
                          &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
