// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vocal Balance against its bars.
//
// The reference is the detector's own arithmetic, stated in the header: for
// a pure tone at a band's frequency the narrow and wide band-passes both
// read it at 0 dB, so the band's excess is 10 log10(selectivity / 0.7) dB,
// the reduction is that over the threshold through the 6 dB knee, times
// depth, capped by the band's cut, and the bell takes exactly that much off
// the tone (the matched peak design is exact at its centre). The tests drive
// that with tones, so the expected figure is analytic.
//
// The other bands are given a cut of 0 so that only the band under test
// acts; the tone sits at its centre.
#include <cmath>
#include <string>
#include <utility>

#include "sonora/dsp/biquad.h"
#include "sonora/measure/measure.h"
#include "sonora/native/vocal_balance.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

constexpr double kPi = 3.14159265358979323846;

double tone_amp(const std::vector<float>& y, double f, double fs, std::size_t from, std::size_t n) {
    double s = 0.0, c = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * f * static_cast<double>(from + i) / fs;
        s += y[from + i] * std::sin(ph);
        c += y[from + i] * std::cos(ph);
    }
    return 2.0 * std::hypot(s, c) / static_cast<double>(n);
}

Chans mix(Chans a, const Chans& b) {
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

// Only band `b` (1..5) acts: the others have a cap of 0.
Config only_band(int b, double freq, std::initializer_list<std::pair<const char*, float>> more = {}) {
    static const char* const kFreq[] = {"freq1", "freq2", "freq3", "freq4", "freq5"};
    static const char* const kCut[] = {"cut1", "cut2", "cut3", "cut4", "cut5"};
    Config c = {{"bands", 2.0f}};
    for (int i = 0; i < 5; ++i) {
        if (i + 1 != b) c.push_back({kCut[i], 0.0f});
    }
    c.push_back({kFreq[b - 1], static_cast<float>(freq)});
    for (const auto& m : more) c.push_back(m);
    return c;
}

// The reduction for a given excess (dB over what the surroundings predict).
double cut_for_excess(double excess, double thr, double depth, double cut) {
    const double x = excess - thr;
    double r = x <= -3.0 ? 0.0 : x < 3.0 ? (x + 3.0) * (x + 3.0) / 12.0 : x;
    r *= depth;
    return r > cut ? cut : r;
}

// A tone alone: the surroundings are floored at 30 dB under the narrow level,
// so the excess is 30 dB less the smooth-spectrum bias 10 log10(r / (1 - r)).
double expected_cut(double sel, double thr, double depth, double cut) {
    const double q = 0.7 / sel;
    return cut_for_excess(30.0 - 10.0 * std::log10(q / (1.0 - q)), thr, depth, cut);
}

// Gain in dB of a settled tone through a configuration.
double tone_gain_db(Suite& s, double fs, double f, double level, const Config& cfg, double seconds = 0.8) {
    auto p = s.make();
    measure::apply(*p, cfg);
    p->prepare(fs, 128, 2);
    const Chans x = measure::sine(fs, f, level, seconds);
    const Chans y = measure::render(*p, x);
    const std::size_t a = static_cast<std::size_t>(0.5 * fs), b = x[0].size();
    return measure::to_db(measure::rms(y[0], a, b) / measure::rms(x[0], a, b));
}

void identity(Suite& s) {
    double worst = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double f : {150.0, 1000.0, 3300.0, 4600.0, 7500.0, 12000.0}) {
            if (f >= 0.45 * fs) continue;
            // Depth 0 makes every reduction 0: a 0 dB bell on every band.
            worst = std::fmax(worst, std::fabs(tone_gain_db(s, fs, f, -12.0, {{"bands", 2.0f}, {"depth", 0.0f}, {"threshold", 0.0f}}, 0.4)));
            // Mix 0 is the dry signal whatever the bands do.
            worst = std::fmax(worst, std::fabs(tone_gain_db(s, fs, f, -12.0, {{"bands", 2.0f}, {"mix", 0.0f}, {"threshold", 0.0f}}, 0.4)));
        }
    }
    s.check_le("identity: depth 0 and mix 0, 3 rates x 6 tones", worst, s.bar("vocal_balance.identity_db"), "dB");
}

void tone_curve(Suite& s) {
    // The reduction of a tone at the band's centre against the stated curve,
    // through selectivity x threshold x depth/cap, at three frequencies.
    const double fs = 48000.0;
    double worst = 0.0;
    std::string where;
    for (int b : {2, 3, 5}) {
        const double f = b == 2 ? 1000.0 : b == 3 ? 3700.0 : 7500.0;
        for (double sel : {3.0, 6.0, 12.0}) {
            for (double thr : {0.0, 6.0, 12.0}) {
                for (const auto& [depth, cap] : {std::pair<double, double>{100.0, 9.0}, {10.0, 9.0}}) {
                    const double cap_arg = cap;
                    const double got = -tone_gain_db(s, fs, f, -18.0,
                                                     only_band(b, f, {{"selectivity", static_cast<float>(sel)},
                                                                      {"threshold", static_cast<float>(thr)},
                                                                      {"depth", static_cast<float>(depth)},
                                                                      {b == 2 ? "cut2" : b == 3 ? "cut3" : "cut5",
                                                                       static_cast<float>(cap_arg)},
                                                                      {"attack", 1.0f}, {"release", 50.0f}}));
                    const double want = expected_cut(sel, thr, depth * 0.01, cap);
                    if (std::fabs(got - want) > worst) {
                        worst = std::fabs(got - want);
                        where = "f " + std::to_string(f) + " sel " + std::to_string(sel) + " thr " + std::to_string(thr) +
                                " depth " + std::to_string(depth) + ": got " + std::to_string(got) + ", want " +
                                std::to_string(want);
                    }
                }
            }
        }
    }
    s.check_le("tone at a band's centre: |reduction - stated curve|, 3 bands x 3 selectivities x 3 thresholds x 2 depth/caps",
               worst, s.bar("vocal_balance.tone_cut_db"), "dB", where);
}

void bed(Suite& s) {
    const double fs = 48000.0;
    // Flat noise has no peak over its surroundings: the band stays out of it.
    {
        auto p = s.make();
        measure::apply(*p, only_band(3, 3700.0, {{"threshold", 6.0f}, {"depth", 100.0f}}));
        p->prepare(fs, 128, 2);
        const Chans x = measure::noise(fs, -20.0, 3.0, 7);
        const Chans y = measure::render(*p, x);
        const std::size_t from = static_cast<std::size_t>(0.5 * fs);
        const auto bx = measure::octave_band(x[0], fs, 3700.0), by = measure::octave_band(y[0], fs, 3700.0);
        double sx = 0.0, sy = 0.0;
        for (std::size_t i = from; i < bx.size(); ++i) {
            sx += bx[i] * bx[i];
            sy += by[i] * by[i];
        }
        s.check_le("flat noise: the band around the 3.7 kHz band |gain|", std::fabs(10.0 * std::log10(sy / sx)),
                   s.bar("vocal_balance.noise_db"), "dB");
    }
    // A tone on a white bed, a stated number of dB over the noise in the
    // narrow band-pass: the excess is 10 log10(1 + T / (rho Bn)), where the
    // noise power in the narrow band is rho (its density, one-sided) times
    // the band-pass's equivalent noise bandwidth Bn, integrated here from the
    // very filter the processor designs.
    for (const auto& [excess_db, thr] : {std::pair<double, double>{12.0, 3.0}, {4.0, 3.0}}) {
        const double sel = 6.0, f = 3700.0, noise_peak = -30.0;
        const auto bp = dsp::design_matched({dsp::Shape::BandPass, f, sel, 0.0}, fs);
        double bn = 0.0;
        for (double g = 0.5; g < 0.5 * fs; g += 1.0) {
            const double h = std::abs(dsp::response(bp, g, fs));
            bn += h * h;
        }
        const double rms = std::pow(10.0, noise_peak / 20.0) / std::sqrt(3.0);
        const double rho = rms * rms / (0.5 * fs);
        const double tone_ms = rho * bn * (std::pow(10.0, excess_db / 10.0) - 1.0);
        const double tone_db = 20.0 * std::log10(std::sqrt(2.0 * tone_ms));
        auto p = s.make();
        measure::apply(*p, only_band(3, f, {{"selectivity", static_cast<float>(sel)}, {"threshold", static_cast<float>(thr)},
                                             {"depth", 100.0f}, {"cut3", 12.0f}, {"attack", 5.0f}, {"release", 60.0f}}));
        p->prepare(fs, 128, 2);
        const Chans x = mix(measure::sine(fs, f, tone_db, 3.0), measure::noise(fs, noise_peak, 3.0, 11));
        const Chans y = measure::render(*p, x);
        const std::size_t from = static_cast<std::size_t>(1.0 * fs), n = static_cast<std::size_t>(1.0 * fs);
        const double red = -measure::to_db(tone_amp(y[0], f, fs, from, n) / tone_amp(x[0], f, fs, from, n));
        const double want = cut_for_excess(excess_db, thr, 1.0, 12.0);
        s.check_le("tone " + std::to_string(excess_db) + " dB over the noise in the narrow band, threshold " +
                       std::to_string(thr) + ": |reduction - stated curve|",
                   std::fabs(red - want), s.bar("vocal_balance.bed_db"), "dB",
                   "measured " + std::to_string(red) + " dB, stated " + std::to_string(want) + " dB");
    }
}

void bell_width(Suite& s) {
    // Tone A triggers the band; probe B, 1.25 x higher and 20 dB lower, shows
    // the bell's width: its gain must be the matched peak design's at the
    // measured reduction.
    const double fs = 48000.0;
    const double fa = 3700.0, fb = 4625.0;
    double worst = 0.0;
    std::string where;
    for (double q : {1.5, 4.0, 12.0}) {
        auto p = s.make();
        measure::apply(*p, only_band(3, fa, {{"sharpness", static_cast<float>(q)}, {"threshold", 0.0f}, {"depth", 100.0f},
                                              {"cut3", 9.0f}, {"attack", 1.0f}}));
        p->prepare(fs, 128, 2);
        const Chans x = mix(measure::sine(fs, fa, -18.0, 1.0), measure::sine(fs, fb, -38.0, 1.0));
        const Chans y = measure::render(*p, x);
        const std::size_t from = static_cast<std::size_t>(0.5 * fs), n = static_cast<std::size_t>(0.4 * fs);
        const double ga = tone_amp(y[0], fa, fs, from, n) / tone_amp(x[0], fa, fs, from, n);
        const double gb = tone_amp(y[0], fb, fs, from, n) / tone_amp(x[0], fb, fs, from, n);
        const double red = -measure::to_db(ga);
        const auto c = dsp::design_matched({dsp::Shape::Peak, fa, q, -red}, fs);
        const double want = measure::to_db(std::abs(dsp::response(c, fb, fs)));
        const double err = std::fabs(measure::to_db(gb) - want);
        if (err > worst) {
            worst = err;
            where = "Q " + std::to_string(q) + ": probe " + std::to_string(measure::to_db(gb)) + " dB, bell " +
                    std::to_string(want) + " dB at a " + std::to_string(red) + " dB cut";
        }
    }
    s.check_le("sharpness: probe 1.25 x above the band vs the matched bell at the measured cut, Q 1.5, 4, 12", worst,
               s.bar("vocal_balance.bell_db"), "dB", where);
}

void delta_and_mix(Suite& s) {
    const double fs = 48000.0;
    const Chans x = mix(mix(measure::sine(fs, 3700.0, -24.0, 1.5), measure::sine(fs, 1000.0, -18.0, 1.5)),
                        measure::noise(fs, -40.0, 1.5, 5));
    const Config base = {{"bands", 2.0f}, {"threshold", 0.0f}, {"depth", 100.0f}, {"freq3", 3700.0f}, {"freq2", 1000.0f}};
    auto run = [&](Config extra) {
        Config c = base;
        for (const auto& e : extra) c.push_back(e);
        auto p = s.make();
        measure::apply(*p, c);
        p->prepare(fs, 128, 2);
        return measure::render(*p, x);
    };
    const Chans wet = run({});
    const Chans delta = run({{"listen", 1.0f}});
    double worst = 0.0, moved = 0.0;
    for (std::size_t i = 0; i < x[0].size(); ++i) {
        worst = std::fmax(worst, std::fabs(static_cast<double>(wet[0][i]) + delta[0][i] - x[0][i]));
        moved = std::fmax(moved, std::fabs(static_cast<double>(delta[0][i])));
    }
    s.check_le("delta: processed + delta - input, peak", worst, s.bar("vocal_balance.delta_null"), "FS",
               "the delta itself peaks at " + std::to_string(moved));
    s.check("delta is audible: the bands removed something", moved, s.bar("vocal_balance.delta_min"), moved >= s.bar("vocal_balance.delta_min"),
            "FS");
    // Mix 50 %: the tone at the band's centre is the average of dry and wet.
    const Chans half = run({{"mix", 50.0f}});
    const std::size_t from = static_cast<std::size_t>(0.8 * fs), n = static_cast<std::size_t>(0.6 * fs);
    const double g_wet = tone_amp(wet[0], 3700.0, fs, from, n) / tone_amp(x[0], 3700.0, fs, from, n);
    const double g_half = tone_amp(half[0], 3700.0, fs, from, n) / tone_amp(x[0], 3700.0, fs, from, n);
    s.check_le("mix 50 %: tone gain vs (1 + wet gain) / 2", std::fabs(measure::to_db(g_half) - measure::to_db(0.5 * (1.0 + g_wet))),
               s.bar("vocal_balance.mix_db"), "dB", "wet gain " + std::to_string(measure::to_db(g_wet)) + " dB");
}

void solo(Suite& s) {
    const double fs = 48000.0;
    auto level = [&](double f) {
        auto p = s.make();
        measure::apply(*p, {{"bands", 2.0f}, {"listen", 2.0f}});
        p->prepare(fs, 128, 2);
        const Chans x = measure::sine(fs, f, -12.0, 0.5);
        const Chans y = measure::render(*p, x);
        const std::size_t a = static_cast<std::size_t>(0.3 * fs);
        return measure::to_db(measure::rms(y[0], a, y[0].size()) / measure::rms(x[0], a, x[0].size()));
    };
    s.check_le("solo: a tone at band 2's centre passes, |gain|", std::fabs(level(1000.0)), s.bar("vocal_balance.solo_pass_db"), "dB");
    s.check("solo: 100 Hz is outside every band", level(100.0), s.bar("vocal_balance.solo_stop_db"),
            level(100.0) <= s.bar("vocal_balance.solo_stop_db"), "dB");
}

// Attack and release as differences. The detector's windows (5 ms and
// longer) and its gate (a level under -80 dBFS reads as no resonance, which
// is how a tone is taken away here) add a fixed lag to both; raising the
// time by a known amount must raise the measured 63 % time by about as much.
double t63(const std::vector<double>& red_db, double fs, std::size_t from, std::size_t to, bool rising) {
    const double end = rising ? red_db[to - 1] : red_db[from];
    std::size_t i = from;
    const double goal = end * (1.0 - std::exp(-1.0));
    if (rising) {
        while (i < to && red_db[i] < goal) ++i;
    } else {
        const double stay = end * std::exp(-1.0);
        while (i < to && red_db[i] > stay) ++i;
    }
    return static_cast<double>(i - from) / fs;
}

void timing(Suite& s) {
    const double fs = 48000.0;
    auto trace = [&](float attack, float release, double& t_att, double& t_rel) {
        auto p = s.make();
        measure::apply(*p, only_band(3, 3700.0, {{"threshold", 0.0f}, {"depth", 100.0f}, {"cut3", 9.0f},
                                                  {"attack", attack}, {"release", release}}));
        p->prepare(fs, 128, 2);
        const double on = 0.2, hold = 0.6, tail = 1.6;
        const Chans x = measure::concat({measure::sine(fs, 3700.0, -90.0, on), measure::sine(fs, 3700.0, -20.0, hold),
                                         measure::sine(fs, 3700.0, -90.0, tail)});
        const Chans y = measure::render(*p, x);
        const std::size_t w = static_cast<std::size_t>(0.0015 * fs);
        std::vector<double> red(x[0].size(), 0.0);
        for (std::size_t i = w; i < red.size(); ++i) {
            const double a = measure::rms(y[0], i - w, i), b = measure::rms(x[0], i - w, i);
            red[i] = b > 0.0 ? -measure::to_db(a / b) : 0.0;
        }
        const std::size_t i_on = static_cast<std::size_t>(on * fs), i_off = i_on + static_cast<std::size_t>(hold * fs);
        t_att = t63(red, fs, i_on + w, i_off, true);
        t_rel = t63(red, fs, i_off - 1, red.size(), false);
    };
    double a_fast, r_fast, a_slow, r_slow, unused;
    trace(0.5f, 10.0f, a_fast, r_fast);
    trace(40.0f, 10.0f, a_slow, unused);
    trace(0.5f, 300.0f, unused, r_slow);
    const double da = (a_slow - a_fast) / 39.5e-3, dr = (r_slow - r_fast) / 290e-3;
    s.check_le("attack: (t63 at 40 ms - t63 at 0.5 ms) / 39.5 ms, |ratio - 1|", std::fabs(da - 1.0), s.bar("vocal_balance.time_tolerance"),
               "ratio", "t63 " + std::to_string(a_fast * 1e3) + " ms at 0.5, " + std::to_string(a_slow * 1e3) + " ms at 40");
    s.check_le("release: (t63 at 300 ms - t63 at 10 ms) / 290 ms, |ratio - 1|", std::fabs(dr - 1.0), s.bar("vocal_balance.time_tolerance"),
               "ratio", "t63 " + std::to_string(r_fast * 1e3) + " ms at 10, " + std::to_string(r_slow * 1e3) + " ms at 300");
}

// The harness's 100 Hz zipper tone reaches no band's narrow detector's
// excess, so its sweeps prove the coefficient ramps at no reduction. This
// tone sits on band 2 with a cut in progress, and the sweeps read the HF
// residue above 5 kHz as the standard check does, its bar moved by the tone's
// 6 dB.
void zipper_sweeps(Suite& s) {
    const double fs = 48000.0;
    const char* ids[] = {"depth", "sharpness", "selectivity", "threshold", "attack", "release", "mix", "output",
                         "freq1", "freq2", "freq3", "cut1", "cut2", "cut3"};
    Chans in = measure::sine(fs, 1000.0, -12.0, 3.25);
    fade_in(in, fs);
    for (const char* id : ids) {
        auto p = s.make();
        measure::apply(*p, {{"bands", 0.0f}, {"freq2", 1000.0f}, {"threshold", 0.0f}, {"depth", 100.0f}, {"attack", 5.0f}, {"release", 60.0f}});
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
        s.check_le(std::string("zipper (1 kHz on band 2): full-range sweep of '") + id + "', HF residue peak",
                   measure::to_db(measure::hf_residue_peak(out[0], fs, start)), s.bar("zipper_dbfs") - 6.0, "dBFS");
    }
}

}  // namespace

measure::Config vocal_balance_golden_config() {
    return {{"bands", 2.0f}, {"depth", 100.0f}, {"threshold", 2.0f}, {"selectivity", 4.0f}, {"sharpness", 3.0f},
            {"attack", 2.0f}, {"release", 40.0f}, {"freq1", 300.0f}, {"freq2", 1200.0f}, {"freq3", 3500.0f},
            {"freq4", 5000.0f}, {"freq5", 8000.0f}};
}

int vocal_balance(const measure::Args& args) {
    Suite s("vocal_balance", [] { return make_vocal_balance(); }, args);
    identity(s);
    tone_curve(s);
    bed(s);
    bell_width(s);
    delta_and_mix(s);
    solo(s);
    timing(s);
    zipper_sweeps(s);
    const Config golden = vocal_balance_golden_config();
    measure::check_latency(s, {{"default", {}}, {"delta", {{"listen", 1.0f}}}, {"3 bands", {{"bands", 0.0f}}}});
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, golden, " (100 Hz tone: the coefficient ramps)");
    measure::check_golden(s, golden);
    measure::check_cpu(s, golden, "");
    return s.finish();
}

}  // namespace sonora::native::suites
