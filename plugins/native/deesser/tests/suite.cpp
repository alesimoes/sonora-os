// SPDX-License-Identifier: GPL-3.0-or-later
//
// The de-esser against its bars: the identity at zero reduction, the static
// curve against the Giannoulis et al. gain computer, attack and release
// from step tests, the band against the wide mode (the body untouched or
// not), zipper sweeps that reach the sibilance band, and the common checks.
//
// The detector hears only its band, so the harness's DC step tests and its
// 100 Hz zipper tone cannot drive it; these tests use a tone at the
// frequency setting, where the band-pass has 0 dB gain and the bell takes
// its full cut.
#include <cmath>
#include <string>
#include <tuple>
#include <utility>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/deesser.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

constexpr double kPi = 3.14159265358979323846;
constexpr double kSib = 6500.0;  // the default frequency, and the test tone

// Amplitude of the component at f over [from, from + n), by projection on
// a sine and a cosine; n must hold a whole number of its cycles.
double tone_amp(const std::vector<float>& y, double f, double fs, std::size_t from, std::size_t n) {
    double s = 0.0, c = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * f * static_cast<double>(from + i) / fs;
        s += y[from + i] * std::sin(ph);
        c += y[from + i] * std::cos(ph);
    }
    return 2.0 * std::hypot(s, c) / static_cast<double>(n);
}

Chans two_tones(double fs, double f1, double db1, double f2, double db2, double seconds) {
    Chans a = measure::sine(fs, f1, db1, seconds);
    const Chans b = measure::sine(fs, f2, db2, seconds);
    for (std::size_t c = 0; c < a.size(); ++c) {
        for (std::size_t i = 0; i < a[c].size(); ++i) a[c][i] += b[c][i];
    }
    return a;
}

void fade_in(Chans& x, double fs) {
    const auto f = static_cast<std::size_t>(0.01 * fs);
    for (auto& ch : x) {
        for (std::size_t i = 0; i < f; ++i) ch[i] *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * i / f));
    }
}

// Gain of a wide-mode render as a trace: out / in wherever the input is
// not near zero, held between those samples. The tone is -20 dBFS inside
// [on, off) and -60 outside.
std::vector<double> gain_trace(const Chans& y, const Chans& x, double on, double off) {
    std::vector<double> g(x[0].size(), 1.0);
    double last = 1.0;
    for (std::size_t i = 0; i < g.size(); ++i) {
        const double amp = (i >= on && i < off) ? 0.1 : 0.001;
        if (std::fabs(x[0][i]) > 0.5 * amp) last = y[0][i] / x[0][i];
        g[i] = last;
    }
    return g;
}

void identity(Suite& s) {
    // Threshold at 0 dB and ratio 1: no reduction, a 0 dB bell, which must
    // leave every frequency alone, at three rates and three settings.
    double worst = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double fc : {2000.0, 6500.0, 16000.0}) {
            for (double f : {100.0, 700.0, 1900.0, 2100.0, 4000.0, 6400.0, 6600.0, 9000.0, 15000.0, 19000.0}) {
                if (f >= 0.45 * fs) continue;
                auto p = s.make();
                measure::apply(*p, {{"frequency", static_cast<float>(fc)}, {"threshold", 0.0f}, {"ratio", 1.0f}});
                p->prepare(fs, 128, 2);
                const Chans x = measure::sine(fs, f, -6.0, 0.15);
                const Chans y = measure::render(*p, x);
                const std::size_t a = static_cast<std::size_t>(0.05 * fs), b = x[0].size();
                const double db = measure::to_db(measure::rms(y[0], a, b) / measure::rms(x[0], a, b));
                worst = std::fmax(worst, std::fabs(db));
            }
        }
    }
    s.check_le("identity: magnitude with no reduction, 3 rates x 3 frequencies x 10 tones", worst,
               s.bar("deesser.identity_db"), "dB");
}

void static_curve(Suite& s) {
    const double fs = 48000.0;
    double worst[2] = {0.0, 0.0};
    for (int mode = 0; mode < 2; ++mode) {
        for (const auto& [ratio, knee] : {std::pair<float, float>{4.0f, 6.0f}, {20.0f, 0.0f}, {1.5f, 12.0f}}) {
            for (double level = -60.0; level <= 0.0; level += 3.0) {
                auto p = s.make();
                measure::apply(*p, {{"mode", static_cast<float>(mode)}, {"frequency", static_cast<float>(kSib)},
                                    {"threshold", -35.0f}, {"ratio", ratio}, {"knee", knee}, {"range", 18.0f},
                                    {"attack", 1.0f}, {"release", 20.0f}});
                p->prepare(fs, 128, 2);
                const Chans x = measure::sine(fs, kSib, level, 0.6);
                const Chans y = measure::render(*p, x);
                const std::size_t a = static_cast<std::size_t>(0.4 * fs), b = x[0].size();
                const double got = measure::to_db(measure::rms(y[0], a, b) / measure::rms(x[0], a, b));
                double red = level - dsp::compressor_curve_db(level, -35.0, ratio, knee);
                if (red > 18.0) red = 18.0;
                worst[mode] = std::fmax(worst[mode], std::fabs(got + red));
            }
        }
    }
    s.check_le("static gain at the centre vs the Giannoulis curve, band mode, range-limited, 3 curves x 21 levels",
               worst[0], s.bar("deesser.static_db"), "dB");
    s.check_le("static gain vs the Giannoulis curve, wide mode, range-limited, 3 curves x 21 levels", worst[1],
               s.bar("deesser.static_db"), "dB");
}

void timing(Suite& s) {
    const double tol = s.bar("deesser.time_tolerance");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto& [a, r] : {std::pair<float, float>{5.0f, 50.0f}, {20.0f, 200.0f}}) {
            auto p = s.make();
            measure::apply(*p, {{"mode", 1.0f}, {"frequency", static_cast<float>(kSib)}, {"threshold", -40.0f},
                                {"ratio", 20.0f}, {"knee", 0.0f}, {"range", 40.0f}, {"attack", a},
                                {"release", r}});
            p->prepare(fs, 128, 2);
            const double hold = 0.25 + 12.0 * a * 1e-3 + 0.2;
            const Chans x = measure::concat({measure::sine(fs, kSib, -60.0, 0.3), measure::sine(fs, kSib, -20.0, hold),
                                             measure::sine(fs, kSib, -60.0, 1.5 + 10.0 * r * 1e-3)});
            const Chans y = measure::render(*p, x);
            const auto on = static_cast<std::size_t>(0.3 * fs);
            const auto off = on + static_cast<std::size_t>(hold * fs);
            const std::vector<double> g = gain_trace(y, x, static_cast<double>(on), static_cast<double>(off));
            // Settled reduction just before the drop.
            const double gr_end = -20.0 * std::log10(g[off - 1]);
            const std::string tag = " (a " + std::to_string(a) + ", r " + std::to_string(r) + ") at " +
                                    std::to_string(static_cast<int>(fs)) + " Hz";
            std::size_t i = on;
            while (i < off && -20.0 * std::log10(g[i]) < gr_end * (1.0 - std::exp(-1.0))) ++i;
            const double ta = static_cast<double>(i - on) / fs;
            s.check_le("attack |measured/set - 1|" + tag, std::fabs(ta / (a * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(ta * 1e3) + " ms, reduction " + std::to_string(gr_end) + " dB");
            std::size_t k = off;
            while (k < g.size() && -20.0 * std::log10(g[k]) > gr_end * std::exp(-1.0)) ++k;
            const double tr = static_cast<double>(k - off) / fs;
            s.check_le("release |measured/set - 1|" + tag, std::fabs(tr / (r * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(tr * 1e3) + " ms");
        }
    }
}

void band_vs_wide(Suite& s) {
    const double fs = 48000.0;
    // A loud sibilant over a 200 Hz body; each one's level through each
    // mode, by projection over whole cycles.
    const Chans x = two_tones(fs, 200.0, -12.0, kSib, -12.0, 1.0);
    const std::size_t from = static_cast<std::size_t>(0.6 * fs), n = static_cast<std::size_t>(0.2 * fs);
    const double in200 = tone_amp(x[0], 200.0, fs, from, n);
    const double insib = tone_amp(x[0], kSib, fs, from, n);
    double body[2], sib[2];
    for (int mode = 0; mode < 2; ++mode) {
        auto p = s.make();
        measure::apply(*p, {{"mode", static_cast<float>(mode)}, {"frequency", static_cast<float>(kSib)},
                            {"threshold", -40.0f}, {"ratio", 20.0f}, {"knee", 0.0f}, {"range", 20.0f},
                            {"attack", 1.0f}, {"release", 20.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, x);
        body[mode] = measure::to_db(tone_amp(y[0], 200.0, fs, from, n) / in200);
        sib[mode] = measure::to_db(tone_amp(y[0], kSib, fs, from, n) / insib);
    }
    s.check_le("band mode leaves the 200 Hz body alone under 20 dB of reduction", std::fabs(body[0]),
               s.bar("deesser.body_db"), "dB", "measured " + std::to_string(body[0]) + " dB");
    s.check_le("band mode cuts the sibilant by the range, |measured + 20|", std::fabs(sib[0] + 20.0),
               s.bar("deesser.band_db"), "dB", "measured " + std::to_string(sib[0]) + " dB");
    s.check_le("wide mode ducks the 200 Hz body by the same reduction, |measured + 20|", std::fabs(body[1] + 20.0),
               s.bar("deesser.band_db"), "dB", "measured " + std::to_string(body[1]) + " dB");
}

// ADR-0011 §3.2's zipper bar on signals the detector hears. The standard
// 100 Hz tone does not reach the sibilance band, so the standard sweep
// proves only the coefficient ramps at zero reduction.
//
// Band mode: a 2 kHz tone (the band's lowest frequency) at -12 dBFS under
// a bell and a detector sitting on it, read by the standard HF residue
// above 5 kHz, whose floor is 2 kHz's own skirt. The bar is the standard
// -60 dBFS moved down by the tone's 6 dB, as the harness does for a
// quieter tone.
//
// Wide mode: a 100 Hz tone at -6 dBFS carries a -20 dBFS 8 kHz sibilant,
// the reduction is applied to the whole signal, and the gain out/in is
// read sample by sample. A step of gain d on the 0.5 peak tone is a click
// of d * 0.5, which is the figure compared with the bar.
void zipper_sweeps(Suite& s, const Config& band_cfg, const Config& wide_cfg) {
    const double fs = 48000.0;
    const char* ids[] = {"frequency", "q", "threshold", "ratio", "knee", "range", "attack", "release", "link", "output"};
    Chans band_in = measure::sine(fs, 2000.0, -12.0, 3.25);
    fade_in(band_in, fs);
    Chans wide_in = two_tones(fs, 100.0, -6.0, 8000.0, -20.0, 3.25);
    fade_in(wide_in, fs);
    for (int wide = 0; wide < 2; ++wide) {
        const Chans& in = wide ? wide_in : band_in;
        const double bar = s.bar("zipper_dbfs") + (wide ? 0.0 : -6.0);
        for (const char* id : ids) {
            auto p = s.make();
            measure::apply(*p, wide ? wide_cfg : band_cfg);
            p->prepare(fs, 128, 2);
            const std::uint32_t idx = measure::param_index(*p, id);
            const native::ParamInfo& info = p->descriptor().params[idx];
            std::vector<measure::Event> ev;
            const double n0 = native::to_normalized(info, info.def);
            const auto start = static_cast<std::uint64_t>(0.25 * fs);
            for (std::uint64_t f = start; f < in[0].size(); f += 128) {
                const double t = static_cast<double>(f - start) / fs;
                double n;
                if (t < n0) n = n0 - t;
                else if (t < n0 + 1.0) n = t - n0;
                else if (t < 2.0) n = 1.0 - (t - n0 - 1.0);
                else n = n0;
                n = std::fmin(1.0, std::fmax(0.0, n));
                ev.push_back({f, idx, static_cast<float>(native::from_normalized(info, n))});
            }
            const Chans out = measure::render(*p, in, ev);
            double db;
            if (!wide) {
                db = measure::to_db(measure::hf_residue_peak(out[0], fs, start));
            } else {
                double worst = 0.0, prev = 0.0;
                std::size_t prev_i = start;
                bool have = false;
                for (std::size_t i = start; i < in[0].size(); ++i) {
                    if (std::fabs(in[0][i]) < 0.2f) continue;
                    const double g = out[0][i] / in[0][i];
                    if (have) worst = std::fmax(worst, std::fabs(g - prev) / static_cast<double>(i - prev_i));
                    have = true;
                    prev = g;
                    prev_i = i;
                }
                db = measure::to_db(worst * 0.5);
            }
            s.check_le(std::string(wide ? "zipper (sibilant, wide): full-range sweep of '"
                                        : "zipper (2 kHz, band): full-range sweep of '") + id +
                           (wide ? "', gain step as a click" : "', HF residue peak"),
                       db, bar, "dBFS");
        }
    }
}

}  // namespace

measure::Config deesser_golden_config() {
    return {{"frequency", 5000.0f}, {"threshold", -34.0f}, {"ratio", 6.0f}, {"range", 18.0f},
            {"attack", 0.4f}, {"release", 50.0f}};
}

int deesser(const measure::Args& args) {
    Suite s("deesser", [] { return make_deesser(); }, args);
    identity(s);
    static_curve(s);
    timing(s);
    band_vs_wide(s);
    const Config golden = deesser_golden_config();
    zipper_sweeps(s,
                  {{"frequency", 2000.0f}, {"threshold", -30.0f}, {"ratio", 4.0f}, {"range", 12.0f},
                   {"attack", 5.0f}, {"release", 60.0f}},
                  {{"mode", 1.0f}, {"frequency", 6500.0f}, {"threshold", -30.0f}, {"ratio", 4.0f},
                   {"range", 12.0f}, {"attack", 5.0f}, {"release", 60.0f}});
    measure::check_latency(s, {{"band", {{"mode", 0.0f}}}, {"wide", {{"mode", 1.0f}}}});
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, golden, " (100 Hz tone: the coefficient ramps at zero reduction)");
    measure::check_golden(s, golden);
    measure::check_cpu(s, golden, "");
    // Wide mode takes an exp per sample and channel; it is held to the class too.
    Config wide = golden;
    wide.push_back({"mode", 1.0f});
    measure::check_cpu(s, wide, " (wide)");
    return s.finish();
}

}  // namespace sonora::native::suites
