// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transformer against its bars (quality.json).
// - Response: unity in the mid-band at every drive, flat to 20 kHz (the
//   leakage and winding capacitance are not modelled), and a low-field
//   corner at the core's stated one. The reversible core's differential
//   inductance moves with level (common/dsp's test_transformer reports it),
//   so the corner is held to a band at a stated level, not to a first-order
//   curve.
// - What an iron core does (the reference is the physics the model is
//   built from, not a unit): the flux is the integral of the voltage, so
//   distortion falls with frequency (LF first), a halved frequency at 6 dB
//   less level sees the same flux and so the same distortion, and a core
//   with a symmetric loop makes no even harmonics.
// - Aliasing at tones from 1 to 19 kHz (no oversampler: it is measured).
// - Stereo equals mono to the bit, hot input, and the common checks
//   (latency, non-finite, reset, denormals, zipper, golden files, CPU).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/transformer.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[240];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

json::Value jf(double v) { return json::Value::number_float(static_cast<float>(v)); }

double db(double lin) { return 20.0 * std::log10(std::fmax(lin, 1e-30)); }

std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    measure::apply(*p, {{"output", 0.0f}});
    measure::apply(*p, c);
    p->prepare(fs, 128, channels);
    return p;
}

// Amplitude of the component at f in y from `from`, Hann-windowed.
double tone_amp(const std::vector<float>& y, std::size_t from, double fs, double f) {
    const std::size_t n = y.size() - from;
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double w0 = dsp::kTwoPi * f / fs;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(dsp::kTwoPi * static_cast<double>(i) / static_cast<double>(n));
        const double ph = w0 * static_cast<double>(from + i);
        re += w * y[from + i] * std::cos(ph);
        im -= w * y[from + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

// A tone long enough to settle the core and the one-pole behaviour of the
// lowest corner (5 Hz: tau 32 ms), measured over whole periods.
double run_gain_db(const Suite& s, double fs, double f, double dbfs, const Config& c) {
    auto p = measuring(s, fs, 1, c);
    const double settle = std::fmax(0.5, 6.0 / f);
    const Chans y = measure::render(*p, measure::sine(fs, f, dbfs, settle + 1.0, 1));
    return db(tone_amp(y[0], static_cast<std::size_t>(settle * fs), fs, f)) - dbfs;
}

double thd_db(const Suite& s, double fs, double f, double dbfs, const Config& c) {
    auto p = measuring(s, fs, 1, c);
    const double settle = std::fmax(0.5, 8.0 / f);
    const Chans y = measure::render(*p, measure::sine(fs, f, dbfs, settle + 1.0, 1));
    return measure::thdn_db(y[0], static_cast<std::size_t>(settle * fs), fs, f);
}

const char* const kCoreIds[] = {"small", "line", "large"};

// ---- response -----------------------------------------------------------------------

void response(Suite& s) {
    const double fs = 48000.0;
    const double level = s.bar("response.level_dbfs");
    json::Value rows = json::Value::array();
    for (int c = 0; c < 3; ++c) {
        const double fc = transformer::kCores[c].corner_hz;
        const Config cfg = {{"core", static_cast<float>(c)}, {"drive", 0.0f}};
        double worst_mid = 0.0;
        for (double f : {1000.0, 5000.0, 10000.0, 20000.0})
            worst_mid = std::fmax(worst_mid, std::fabs(run_gain_db(s, fs, f, level, cfg)));
        s.check_le(std::string("small-signal gain 1-20 kHz, ") + kCoreIds[c] + " core: worst |dB|", worst_mid,
                   s.bar("response.mid_db"), "dB");
        // The corner: the gain at the stated one, the -3 dB of a first-order high-pass.
        const double at_corner = run_gain_db(s, fs, fc, level, cfg);
        s.check_le(std::string("gain at the stated corner (") + fmt("%.0f Hz", fc) + "), " + kCoreIds[c] +
                       " core: distance from -3.01 dB",
                   std::fabs(at_corner + 3.0103), s.bar("response.corner_db"), "dB", fmt("%+.3f dB", at_corner));
        // A high-pass: the gain must rise monotonically through the corner's
        // decades.
        double prev = -400.0;
        bool mono = true;
        for (double mult : {0.25, 0.5, 1.0, 2.0, 4.0, 16.0}) {
            const double g = run_gain_db(s, fs, fc * mult, level, cfg);
            mono &= g > prev;
            prev = g;
            json::Value r = json::Value::object();
            r.set("core", json::Value::integer(c));
            r.set("hz", jf(fc * mult));
            r.set("db", jf(g));
            rows.push(r);
        }
        s.check(std::string("gain rises with frequency from a quarter to 16 times the corner, ") + kCoreIds[c], prev,
                0.0, mono, "dB");
    }
    s.data().set("response", rows);
    // Drive is divided out: unity small-signal gain at 1 kHz at every drive.
    double worst = 0.0;
    for (float drive : {0.0f, 12.0f, 24.0f})
        worst = std::fmax(worst, std::fabs(run_gain_db(s, fs, 1000.0, level, {{"drive", drive}})));
    s.check_le("small-signal gain at 1 kHz, drive 0/12/24 dB: worst |dB|", worst, s.bar("response.mid_db"), "dB");
    // Output is a plain gain.
    const double g = run_gain_db(s, fs, 1000.0, level, {{"drive", 0.0f}, {"output", 6.0f}});
    s.check_le("output +6 dB: gain error", std::fabs(g - 6.0), s.bar("response.mid_db"), "dB");
}

// ---- what iron does ---------------------------------------------------------------

void iron(Suite& s) {
    const double fs = 48000.0;
    const Config cfg = {{"core", 1.0f}, {"drive", 0.0f}};
    json::Value rows = json::Value::array();
    // THD against level at 1 kHz, 100 Hz and 40 Hz: rising with level, and
    // falling with frequency (LF first).
    const double freqs[] = {1000.0, 100.0, 40.0};
    const double levels[] = {-30.0, -18.0, -6.0, 0.0};
    double t[3][4];
    for (int fi = 0; fi < 3; ++fi) {
        for (int li = 0; li < 4; ++li) {
            t[fi][li] = thd_db(s, fs, freqs[fi], levels[li], cfg);
            json::Value r = json::Value::object();
            r.set("hz", jf(freqs[fi]));
            r.set("dbfs", jf(levels[li]));
            r.set("thd_db", jf(t[fi][li]));
            rows.push(r);
        }
    }
    s.data().set("thd", rows);
    for (int fi = 0; fi < 3; ++fi) {
        bool rising = true;
        for (int li = 1; li < 4; ++li) rising &= t[fi][li] > t[fi][li - 1];
        s.check(fmt("THD+N of a line core at %.0f Hz rises with level (-30 to 0 dBFS)", freqs[fi]), t[fi][3], t[fi][0],
                rising, "dB",
                fmt("%.1f, ", t[fi][0]) + fmt("%.1f, ", t[fi][1]) + fmt("%.1f, ", t[fi][2]) + fmt("%.1f dB", t[fi][3]));
    }
    for (int li = 1; li < 4; ++li) {
        bool lf_first = t[0][li] < t[1][li] && t[1][li] < t[2][li];
        s.check(fmt("THD+N falls with frequency (40 Hz > 100 Hz > 1 kHz) at %.0f dBFS", levels[li]), t[2][li] - t[0][li],
                s.bar("iron.lf_first_db"), lf_first && t[2][li] - t[0][li] >= s.bar("iron.lf_first_db"), "dB",
                fmt("40 Hz %.1f, ", t[2][li]) + fmt("100 Hz %.1f, ", t[1][li]) + fmt("1 kHz %.1f dB", t[0][li]));
    }
    // Same flux, same distortion: the flux is V / f, so f/2 at -6.02 dB.
    {
        double worst = 0.0;
        for (double f : {100.0, 200.0}) {
            for (double l : {-6.0, -12.0}) {
                const double a = thd_db(s, fs, f, l, cfg);
                const double b = thd_db(s, fs, f / 2.0, l - 6.0206, cfg);
                worst = std::fmax(worst, std::fabs(a - b));
            }
        }
        s.check_le("equal flux, equal distortion: THD+N at (f, L) vs (f/2, L - 6 dB), 100 and 200 Hz: worst |dB|",
                   worst, s.bar("iron.flux_law_db"), "dB");
    }
    // A symmetric loop makes odd harmonics only.
    {
        auto p = measuring(s, fs, 1, cfg);
        const Chans y = measure::render(*p, measure::sine(fs, 50.0, 0.0, 3.0, 1));
        const std::size_t from = static_cast<std::size_t>(1.0 * fs);
        const double m1 = tone_amp(y[0], from, fs, 50.0);
        const double h2 = db(tone_amp(y[0], from, fs, 100.0) / m1);
        const double h3 = db(tone_amp(y[0], from, fs, 150.0) / m1);
        s.check_le("50 Hz at 0 dBFS, line core: second harmonic", h2, s.bar("iron.h2_dbc"), "dBc",
                   fmt("H3 %.1f dBc", h3));
        s.check("50 Hz at 0 dBFS, line core: third harmonic stands above the second by at least", h3 - h2,
                s.bar("iron.h3_over_h2_db"), h3 - h2 >= s.bar("iron.h3_over_h2_db"), "dB");
    }
    // The core sets where the distortion starts: at 0 dBFS and 40 Hz the
    // small core distorts more than the line core, the line more than the large.
    {
        double v[3];
        for (int c = 0; c < 3; ++c) v[c] = thd_db(s, fs, 40.0, -6.0, {{"core", static_cast<float>(c)}, {"drive", 0.0f}});
        s.check("THD+N at 40 Hz, -6 dBFS: small > line > large", v[0] - v[2], 0.0, v[0] > v[1] && v[1] > v[2], "dB",
                fmt("%.1f, ", v[0]) + fmt("%.1f, ", v[1]) + fmt("%.1f dB", v[2]));
    }
}

// ---- aliasing ---------------------------------------------------------------------

void aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 10007.0, 14009.0, 19001.0};
    struct Lv {
        const char* name;
        float drive;
        double dbfs;
    };
    const Lv levels[] = {{"nominal (drive 6 dB, -12 dBFS)", 6.0f, -12.0}, {"hot (drive 24 dB, 0 dBFS)", 24.0f, 0.0}};
    const char* bars[] = {"aliasing.nominal_dbc", "aliasing.hot_dbc"};
    const std::vector<double> rates =
        measure::quick() ? std::vector<double>{44100.0} : std::vector<double>{44100.0, 48000.0, 96000.0};
    json::Value rows = json::Value::array();
    for (int c = 0; c < 3; ++c) {
        if (measure::quick() && c == 0) continue;
        for (double fs : rates) {
            for (int lv = 0; lv < 2; ++lv) {
                double worst = -400.0, worst_f = 0.0;
                for (double f0 : tones) {
                    if (measure::quick() && f0 != 997.0 && f0 != 5011.0 && f0 != 19001.0) continue;
                    auto p = measuring(s, fs, 1, {{"core", static_cast<float>(c)}, {"drive", levels[lv].drive}});
                    const Chans y = measure::render(*p, measure::sine(fs, f0, levels[lv].dbfs, 1.5, 1));
                    const double a = measure::alias_dbc(y[0], static_cast<std::size_t>(0.6 * fs), fs, f0);
                    if (a > worst) {
                        worst = a;
                        worst_f = f0;
                    }
                    json::Value r = json::Value::object();
                    r.set("core", json::Value::integer(c));
                    r.set("rate", jf(fs));
                    r.set("tone", jf(f0));
                    r.set("level", json::Value::integer(lv));
                    r.set("alias_dbc", jf(a));
                    rows.push(r);
                }
                s.check_le(std::string("aliasing, ") + kCoreIds[c] + ", " + fmt("%.0f Hz, ", fs) + levels[lv].name +
                               ", tones 1-19 kHz: worst non-harmonic",
                           worst, s.bar(bars[lv]), "dBc", fmt("at %.0f Hz", worst_f));
            }
        }
    }
    s.data().set("aliasing", rows);
}

// ---- stereo, hot input -----------------------------------------------------------

void stereo_and_hot(Suite& s) {
    const double fs = 48000.0;
    {
        // The two lanes of the pair solve give the bits the one-channel solve gives.
        Chans x = measure::noise(fs, -6.0, 0.5, 7, 1);
        x.push_back(x[0]);
        auto st = measuring(s, fs, 2, {{"drive", 18.0f}});
        auto mo = measuring(s, fs, 1, {{"drive", 18.0f}});
        const Chans ys = measure::render(*st, x);
        const Chans ym = measure::render(*mo, {x[0]});
        double diff = 0.0;
        for (std::size_t i = 0; i < ym[0].size(); ++i)
            diff = std::fmax(diff, std::fabs(static_cast<double>(ys[0][i]) - ym[0][i]));
        s.check_le("stereo channel against the mono render of the same signal: max |difference|", diff, 0.0, "FS");
    }
    for (int c = 0; c < 3; ++c) {
        double worst = 0.0;
        bool finite = true;
        for (double dbfs : {0.0, 12.0, 24.0}) {
            auto p = measuring(s, fs, 2, {{"core", static_cast<float>(c)}, {"drive", 24.0f}});
            Chans x = measure::silence(fs, 0.3);
            const auto amp = static_cast<float>(std::pow(10.0, dbfs / 20.0));
            for (auto& ch : x)
                for (std::size_t i = 0; i < ch.size(); ++i)
                    ch[i] = std::fmod(30.0 * static_cast<double>(i) / fs, 1.0) < 0.5 ? amp : -amp;
            const Chans y = measure::render(*p, x);
            for (const auto& ch : y)
                for (float v : ch) {
                    finite &= std::isfinite(v);
                    worst = std::fmax(worst, std::fabs(v));
                }
        }
        s.check(std::string("hot 30 Hz squares 0, +12, +24 dBFS, drive 24 dB, ") + kCoreIds[c] +
                    " core: output finite and bounded",
                worst, s.bar("hot.max_abs"), finite && worst <= s.bar("hot.max_abs"), "FS");
    }
}

}  // namespace

measure::Config transformer_golden_config() { return {{"drive", 12.0f}, {"output", -1.0f}}; }

int transformer(const measure::Args& args) {
    Suite s("transformer", [] { return make_transformer(); }, args);
    response(s);
    iron(s);
    aliasing(s);
    stereo_and_hot(s);
    const Config golden = transformer_golden_config();
    std::vector<std::pair<std::string, Config>> latency;
    for (int c = 0; c < 3; ++c) latency.push_back({kCoreIds[c], {{"core", static_cast<float>(c)}}});
    measure::check_common(s, golden, latency);
    return s.finish();
}

}  // namespace sonora::native::suites
