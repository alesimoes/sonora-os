// SPDX-License-Identifier: GPL-3.0-or-later
//
// The limiter against its bars: zero true-peak overs on a stress set, full-
// band white noise included, read both by the BS.1770 4x meter and by a 16x
// reference finer than any detector of ours; transparency below the
// ceiling; release from a step test; aliasing on steady tones; what the
// true-peak guard adds on those tones against the first stage alone; and
// the common checks.
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "sonora/dsp/limiter.h"
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
        // Not band-limited and not faded: full scale up to Nyquist with an
        // abrupt start, the case the look-ahead stage alone let through.
        {"full-band white noise, +12 dB", [](double fs) { return measure::noise(fs, 12.0, 1.0, 99u); }},
        {"full-band white noise, +24 dB", [](double fs) { return measure::noise(fs, 24.0, 1.0, 7u); }},
    };
}

void ceiling(Suite& s) {
    double worst_ref = -1e9, worst_meter = -1e9;
    int overs = 0, runs = 0;
    const std::vector<double> rates = measure::quick() ? std::vector<double>{48000.0}
                                                       : std::vector<double>{44100.0, 48000.0, 96000.0};
    const std::vector<double> ceilings = measure::quick() ? std::vector<double>{-1.0} : std::vector<double>{-1.0, -0.1};
    for (double fs : rates) {
        for (double ceil_db : ceilings) {
            for (const Stress& st : stress()) {
                auto p = s.make();
                measure::apply(*p, {{"ceiling", static_cast<float>(ceil_db)}, {"release", 20.0f}});
                p->prepare(fs, 128, 2);
                // Silence after the signal, so the output holds all of it
                // rather than ending mid-signal in a step of its own.
                const Chans y = measure::render(
                    *p, measure::concat({st.make(fs), measure::silence(fs, (p->latency() + 1024) / fs)}));
                double ref = 0.0, met = 0.0;
                for (const auto& c : y) {
                    ref = std::fmax(ref, measure::reference_true_peak(c, dsp::db_to_gain(ceil_db - 1.0)));
                    met = std::fmax(met, measure::meter_true_peak(c));
                }
                const double m_ref = dsp::gain_to_db(ref) - ceil_db;
                const double m_met = dsp::gain_to_db(met) - ceil_db;
                worst_ref = std::fmax(worst_ref, m_ref);
                worst_meter = std::fmax(worst_meter, m_met);
                if (m_ref > 0.0 || m_met > 0.0) ++overs;
                ++runs;
            }
        }
    }
    s.check("true-peak overs above the ceiling, 16x reference and BS.1770 meter (" + std::to_string(runs) +
                " runs" + (measure::quick() ? ", quick" : "") + ")",
            overs, 0.0, overs == 0, "count");
    s.info("highest output true peak relative to the ceiling, 16x reference", worst_ref, "dB", "target is 0.05 dB under");
    s.info("highest output true peak relative to the ceiling, BS.1770 meter", worst_meter, "dB", "target is 0.05 dB under");
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

// What the true-peak guard adds, on record: THD+N and non-harmonic energy
// of the processor (both stages) against the look-ahead stage alone, at
// the processor's defaults, on tones 6 dB over the ceiling. The guard
// acts by gain, so on a steady tone it can only add the sidebands of its
// own ramps; the aliasing bar above applies to the whole chain.
void guard_cost(Suite& s) {
    const double fs = 48000.0;
    for (double f : {1000.0, 5000.0, 10000.0, 15000.0, 19000.0}) {
        const Chans x = measure::sine(fs, f, 5.0, 1.5);
        auto p = s.make();
        measure::apply(*p, {{"ceiling", -1.0f}});
        p->prepare(fs, 128, 2);
        const Chans chain = measure::render(*p, x);
        dsp::LookaheadLimiter alone;
        alone.prepare(fs, 2, static_cast<std::uint32_t>(std::lround(1.5e-3 * fs)));
        alone.set_ceiling(dsp::db_to_gain(-1.0));
        alone.set_release(0.05);
        Chans first = x;
        for (std::size_t pos = 0; pos < first[0].size(); pos += 128) {
            const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(128, first[0].size() - pos));
            float* io[2] = {&first[0][pos], &first[1][pos]};
            alone.process(io, n);
        }
        const std::string at = std::to_string(static_cast<int>(f)) + " Hz 6 dB over the ceiling";
        s.info("THD+N, " + at + ", both stages", measure::thdn_db(chain[0], 38400, fs, f), "dB",
               "first stage alone " + std::to_string(measure::thdn_db(first[0], 38400, fs, f)) + " dB");
        s.info("non-harmonic energy, " + at + ", both stages", measure::alias_dbc(chain[0], 38400, fs, f), "dBc",
               "first stage alone " + std::to_string(measure::alias_dbc(first[0], 38400, fs, f)) + " dBc");
    }
}

}  // namespace

int limiter(const measure::Args& args) {
    Suite s("limiter", [] { return make_limiter(); }, args);
    ceiling(s);
    transparency(s);
    release(s);
    aliasing(s);
    guard_cost(s);
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
