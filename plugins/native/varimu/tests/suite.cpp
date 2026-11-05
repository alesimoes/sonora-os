// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Vari-Mu against its bars: the cell's static law, the soft knee's
// rising ratio, the switched time constants and their program dependence,
// the stereo link and the side-chain filter, the iron stage, THD, aliasing
// of the gain cell at three rates and qualities, and the common checks.
#include <algorithm>
#include <complex>
#include <cmath>
#include <string>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/varimu.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

const char* const kQualityIds[] = {"eco", "standard", "high"};

// The model's constants, restated here from the circuit's equations
// (varimu.h), not read from the implementation, so a change to the law is
// a change to a test.
constexpr double kUThr = 0.14228643;  // 1 - 10^(-2/30): 2 dB at the threshold
constexpr double kUCeil = 0.95360;    // (1 - u)^1.5 = 0.01
constexpr double kRho = kUThr / (kUCeil - kUThr);

// Reduction in dB (positive) for a peak level `lvl_db` against a threshold.
double law_gr_db(double lvl_db, double thr_db) {
    const double z = kRho * std::pow(10.0, (lvl_db - thr_db) / 20.0);
    const double u = kUCeil * z / (1.0 + z);
    return -30.0 * std::log10(1.0 - u);
}

// The bias, from a reduction in dB (the law inverted), as z.
double z_of_gr(double gr_db) {
    const double u = 1.0 - std::pow(10.0, -gr_db / 30.0);
    return u / (kUCeil - u);
}

// A stereo tone at fs/12 of `n` samples, phase 0: positive peaks fall on
// samples 3 mod 12, so a gain can be read at every peak with no
// interpolation. n is a multiple of 12.
Chans tone12(double, double dbfs, std::size_t n) {
    Chans x(2, std::vector<float>(n));
    const double a = std::pow(10.0, dbfs / 20.0);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = static_cast<float>(a * std::sin(dsp::kTwoPi * static_cast<double>(i % 12) / 12.0));
        x[0][i] = x[1][i] = v;
    }
    return x;
}
std::size_t n12(double fs, double seconds) {
    return static_cast<std::size_t>(std::llround(seconds * fs / 12.0)) * 12;
}

// Reduction in dB at each positive peak of [from, to) of the tone12 input.
std::vector<double> gr_at_peaks(const std::vector<float>& y, const std::vector<float>& x, std::size_t lat,
                                std::size_t from, std::size_t to) {
    std::vector<double> gr;
    for (std::size_t i = from; i + lat < to && i < x.size(); ++i) {
        if (i % 12 != 3 || x[i] <= 0.0f) continue;
        gr.push_back(-20.0 * std::log10(std::fabs(y[i + lat]) / x[i]));
    }
    return gr;
}

double gain_db(const Chans& y, const Chans& x, std::size_t from, std::size_t to) {
    return 20.0 * std::log10(measure::rms(y[0], from, to) / measure::rms(x[0], from, to));
}

Chans stereo_tone(double fs, double freq, double dbfs, double seconds) { return measure::sine(fs, freq, dbfs, seconds); }

Config base_config() { return {{"threshold", -20.0f}, {"time_constant", 0.0f}, {"quality", 0.0f}}; }
Config with(Config c, const Config& more) {
    for (const auto& m : more) c.push_back(m);
    return c;
}

// Steady-state gain of a 1 kHz tone at `level` dBFS, per quality.
double steady_gain_db(Suite& s, const Config& cfg, double level, double fs = 48000.0) {
    auto p = s.make();
    measure::apply(*p, cfg);
    p->prepare(fs, 128, 2);
    const Chans x = stereo_tone(fs, 1000.0, level, 0.8);
    const Chans y = measure::render(*p, x);
    const auto n = x[0].size();
    return gain_db(y, x, n - static_cast<std::size_t>(0.2 * fs), n);
}

// Static curve against the three-halves law, and the ratio the soft knee
// gives: low at the threshold, rising with the reduction.
void static_curve(Suite& s) {
    const double bar = s.bar("static.curve_db");
    const double thr = -20.0;
    double worst = 0.0;
    json::Value pts = json::Value::array();
    for (double level = -50.0; level <= -1.0; level += 3.0) {
        const double want = -law_gr_db(level, thr);
        const double got = steady_gain_db(s, base_config(), level);
        if (-want <= s.bar("static.checked_to_gr_db")) worst = std::fmax(worst, std::fabs(got - want));
        json::Value pt = json::Value::array();
        pt.push(json::Value::number_float(static_cast<float>(level)));
        pt.push(json::Value::number_float(static_cast<float>(got)));
        pt.push(json::Value::number_float(static_cast<float>(want)));
        pts.push(std::move(pt));
    }
    s.data().set("static_curve_level_gain_law", std::move(pts));
    s.check_le("static gain vs the (1-u)^1.5 law, 1 kHz, up to 12 dB of reduction", worst, bar, "dB");

    // Ratio over +-1 dB around three levels.
    struct R {
        double level;
        const char* what;
        double bar;
        bool max;
    };
    for (const R& r : {R{-20.0, "threshold", s.bar("static.ratio_at_threshold_max"), true},
                       R{-9.0, "6 dB of reduction", s.bar("static.ratio_at_6db_min"), false},
                       R{-1.0, "12 dB of reduction", s.bar("static.ratio_at_12db_min"), false}}) {
        const double g0 = steady_gain_db(s, base_config(), r.level - 1.0);
        const double g1 = steady_gain_db(s, base_config(), r.level + 1.0);
        const double ratio = 2.0 / (2.0 + (g1 - g0));
        s.check(std::string("ratio at ") + r.what + " (the soft knee rises with level)", ratio, r.bar,
                r.max ? ratio <= r.bar : ratio >= r.bar, ":1");
    }

    // The deepest reduction: the ceiling of the cell.
    {
        const double g = steady_gain_db(s, with(base_config(), {{"threshold", -50.0f}}), 0.0);
        const double want = law_gr_db(0.0, -50.0);
        s.check("deepest reduction (0 dBFS, threshold -50): under the cell's ceiling and on the law", -g, s.bar("deepest_gr_db"),
                -g <= s.bar("deepest_gr_db") && std::fabs(-g - want) <= s.bar("static.deep_db"), "dB",
                "law " + std::to_string(want) + " dB");
    }
}

// The qualities share one curve (the cell's polynomial is the same; the
// oversampler only band-limits it).
void quality_agreement(Suite& s) {
    const double bar = s.bar("quality_db");
    double worst = 0.0;
    for (double level : {-30.0, -15.0, -5.0}) {
        const double e = steady_gain_db(s, base_config(), level);
        for (int q = 1; q < 3; ++q) {
            if (measure::quick() && q == 2) continue;
            worst = std::fmax(worst, std::fabs(steady_gain_db(s, with(base_config(), {{"quality", float(q)}}), level) - e));
        }
    }
    s.check_le("eco vs standard and high, static gain at -30, -15, -5 dBFS", worst, bar, "dB");
}

// Time from the end of a burst to 63 % of its reduction having gone, and
// the same by the model: tau ln(z0 / z1), the network discharging
// exponentially through the cell's law.
struct Release {
    double gr0 = 0.0, t63 = -1.0;
};

Release release_of(Suite& s, int tc, double fs, double burst_s, double tail_s, double level = -17.0) {
    auto p = s.make();
    measure::apply(*p, with(base_config(), {{"time_constant", static_cast<float>(tc)}}));
    p->prepare(fs, 128, 2);
    const std::size_t pre = n12(fs, 0.1), b = n12(fs, burst_s), t = n12(fs, tail_s);
    const Chans x = measure::concat({tone12(fs, -60.0, pre), tone12(fs, level, b), tone12(fs, -60.0, t)});
    const Chans y = measure::render(*p, x);
    Release r;
    const auto last = gr_at_peaks(y[0], x[0], 0, pre + b - 12 * 6, pre + b);
    for (double g : last) r.gr0 += g / static_cast<double>(last.size());
    const auto tail = gr_at_peaks(y[0], x[0], 0, pre + b, pre + b + t);
    for (std::size_t m = 0; m < tail.size(); ++m) {
        if (tail[m] <= 0.368 * r.gr0) {
            r.t63 = (static_cast<double>(m) * 12.0 + 3.0) / fs;
            break;
        }
    }
    return r;
}

const double kFastRelease[6] = {0.3, 0.8, 2.0, 5.0, 0.3, 0.8};
const double kSlowRelease[6] = {0.0, 0.0, 0.0, 0.0, 3.0, 8.0};

void releases(Suite& s) {
    const double tol = s.bar("release.model_tolerance");
    for (int tc = 0; tc < 4; ++tc) {
        for (double fs : measure::kRates) {
            // Other rates for the shortest only: the coefficients are the
            // same expression of tau and fs for all of them.
            if (tc != 0 && fs != 48000.0) continue;
            if (measure::quick() && tc == 3) continue;
            const double tau = kFastRelease[tc];
            const Release r = release_of(s, tc, fs, 1.0, 3.0 * tau + 0.5);
            const double want = tau * std::log(z_of_gr(r.gr0) / z_of_gr(0.368 * r.gr0));
            s.check_le("release " + std::to_string(tau) + " s (position " + std::to_string(tc + 1) + ") at " +
                           std::to_string(static_cast<int>(fs)) + " Hz, |measured / model - 1|",
                       std::fabs(r.t63 / want - 1.0), tol, "ratio",
                       "measured " + std::to_string(r.t63) + " s, model " + std::to_string(want) + " s from " +
                           std::to_string(r.gr0) + " dB");
        }
    }
}

void program_dependence(Suite& s) {
    const double fs = 48000.0;
    const double fixed_tol = s.bar("program.fixed_tolerance");
    {
        const Release a = release_of(s, 2, fs, 0.1, 8.0), b = release_of(s, 2, fs, 8.0, 8.0);
        s.check_le("fixed position 3: release after 0.1 s and after 8 s agree, |ratio - 1|",
                   std::fabs(b.t63 / a.t63 - 1.0), fixed_tol, "ratio",
                   "0.1 s: " + std::to_string(a.t63) + " s, 8 s: " + std::to_string(b.t63) + " s");
    }
    for (int tc = 4; tc < 6; ++tc) {
        if (measure::quick() && tc == 5) continue;
        const double slow = kSlowRelease[tc];
        // The slow network keeps charging from the fast one while it
        // releases, so after a short peak the tail still reaches into the
        // slow network's time: the window covers it.
        const Release a = release_of(s, tc, fs, 0.1, 3.0 * slow + 1.0);
        const Release b = release_of(s, tc, fs, 12.0, 3.0 * slow + 1.0);
        s.check("program position " + std::to_string(tc + 1) + ": release after 12 s over after 0.1 s",
                b.t63 / a.t63, s.bar("program.long_over_short_min"), a.t63 > 0.0 && b.t63 / a.t63 >= s.bar("program.long_over_short_min"), "ratio",
                "0.1 s: " + std::to_string(a.t63) + " s, 12 s: " + std::to_string(b.t63) + " s");
        // After a short peak the fast network sets the release.
        // The slow network has taken a part of the level by then (it
        // charges with tau 0.5 s), so the tail is longer than the fast
        // network's alone, by a bounded factor (position 5; position 6's
        // is reported, its slow network being 8 s).
        if (tc != 4) {
            s.info("program position " + std::to_string(tc + 1) + ": release after 0.1 s, t63 / tau", a.t63 / kFastRelease[tc], "ratio");
            continue;
        }
        s.check_le("program position " + std::to_string(tc + 1) + ": release after 0.1 s near the fast network, t63 / tau",
                   a.t63 / kFastRelease[tc], s.bar("program.short_over_tau_max"), "ratio");
    }
}

// Attack: the rectifier charges on the peaks only, so the 63 % time of a
// tone is a multiple of tau (the bars say which).
void attacks(Suite& s) {
    const double fs = 48000.0;
    double prev = 0.0;
    for (int tc = 0; tc < 4; ++tc) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"time_constant", static_cast<float>(tc)}}));
        p->prepare(fs, 128, 2);
        const std::size_t pre = n12(fs, 0.1), b = n12(fs, 0.4);
        const Chans x = measure::concat({tone12(fs, -60.0, pre), tone12(fs, -9.0, b)});
        const Chans y = measure::render(*p, x);
        const auto tr = gr_at_peaks(y[0], x[0], 0, pre, pre + b);
        double fin = 0.0;
        for (std::size_t i = tr.size() - 20; i < tr.size(); ++i) fin += tr[i] / 20.0;
        double t63 = -1.0;
        for (std::size_t m = 0; m < tr.size(); ++m) {
            if (tr[m] >= 0.632 * fin) {
                t63 = (static_cast<double>(m) * 12.0 + 3.0) / fs;
                break;
            }
        }
        static const double tau[4] = {0.0002, 0.0002, 0.0004, 0.0008};
        const double ratio = t63 / tau[tc];
        s.check("attack position " + std::to_string(tc + 1) + ": 63 % of " + std::to_string(fin) + " dB over tau",
                ratio, s.bar("attack.t63_over_tau_max"),
                ratio >= s.bar("attack.t63_over_tau_min") && ratio <= s.bar("attack.t63_over_tau_max"), "ratio",
                "t63 " + std::to_string(t63 * 1e3) + " ms");
        if (tc == 2) prev = t63;
        if (tc == 3) s.check("attack lengthens with the position (3 -> 4)", t63, prev, t63 > prev, "s");
    }
}

void thd(Suite& s) {
    const double fs = 48000.0;
    json::Value pts = json::Value::array();
    struct T {
        double level;
        float thr;
        const char* what;
        const char* bar;
    };
    for (const T& t : {T{-18.0, 0.0f, "no reduction", "thd.clean_db"}, T{-12.0, -30.0f, "about 10 dB of reduction", "thd.reduced_db"}}) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"threshold", t.thr}, {"time_constant", 3.0f}}));
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, stereo_tone(fs, 1000.0, t.level, 1.2));
        const double v = measure::thdn_db(y[0], 19200, fs, 1000.0);
        s.check_le(std::string("THD+N, 1 kHz at ") + std::to_string(static_cast<int>(t.level)) + " dBFS, " + t.what, v, s.bar(t.bar), "dB");
    }
    // The even term grows with the reduction: the second harmonic of the
    // reduced case is above the clean case's (the cell works nearer cutoff).
    auto thd_of = [&](double level, float thr) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"threshold", thr}, {"time_constant", 3.0f}}));
        p->prepare(fs, 128, 2);
        return measure::thdn_db(measure::render(*p, stereo_tone(fs, 1000.0, level, 1.2))[0], 19200, fs, 1000.0);
    };
    const double rise = thd_of(-12.0, -30.0f) - thd_of(-12.0, 0.0f);
    s.check("distortion rises with the reduction (-12 dBFS, threshold -30 over 0)", rise, 0.0, rise > 0.0, "dB");
}

// The gain cell's aliasing at three rates and qualities: a tone near the
// top of the band, whose third harmonic (the cubic) and second (the even
// term, folding for tones above fs/4) fall outside the base band.
void aliasing(Suite& s) {
    for (double fs : measure::kRates) {
        if (measure::quick() && fs == 48000.0) continue;
        for (int q = 0; q < 3; ++q) {
            if (measure::quick() && q == 2) continue;
            for (double f0 : {10000.0, 12000.0}) {
                auto p = s.make();
                measure::apply(*p, with(base_config(), {{"time_constant", 2.0f}, {"quality", float(q)}}));
                p->prepare(fs, 128, 2);
                const Chans y = measure::render(*p, stereo_tone(fs, f0, -18.0, 1.2));
                const double v = measure::alias_dbc(y[0], 19200, fs, f0);
                const std::string key = std::string("aliasing.") + kQualityIds[q] + "_dbc";
                s.check_le(std::string("aliasing of the gain cell, ") + std::to_string(static_cast<int>(f0)) + " Hz at -18 dBFS, " +
                               kQualityIds[q] + ", " + std::to_string(static_cast<int>(fs)) + " Hz",
                           v, s.bar(key), "dBc");
            }
        }
    }
    // A hotter tone: reported.
    for (int q = 0; q < 3; ++q) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"time_constant", 2.0f}, {"quality", float(q)}, {"threshold", -10.0f}}));
        p->prepare(44100.0, 128, 2);
        const Chans y = measure::render(*p, stereo_tone(44100.0, 10000.0, -6.0, 1.2));
        s.info(std::string("aliasing, 10 kHz at -6 dBFS, ") + kQualityIds[q] + ", 44100 Hz", measure::alias_dbc(y[0], 19200, 44100.0, 10000.0), "dBc",
               "heavier setting; reported");
    }
}

double hp_mag(double fc, double f, double fs) {
    const double a = 1.0 - std::exp(-dsp::kTwoPi * fc / fs);
    const std::complex<double> z = std::polar(1.0, -dsp::kTwoPi * f / fs);
    return std::abs(1.0 - a / (1.0 - (1.0 - a) * z));
}

void response(Suite& s) {
    const double fs = 48000.0;
    auto gain_at = [&](double f, float iron) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"threshold", 0.0f}, {"time_constant", 3.0f}, {"iron", iron}}));
        p->prepare(fs, 128, 2);
        const Chans x = stereo_tone(fs, f, -40.0, 1.5);
        const Chans y = measure::render(*p, x);
        const auto n = x[0].size();
        return gain_db(y, x, n - static_cast<std::size_t>(0.5 * fs), n);
    };
    const double ref = gain_at(1000.0, 0.0f);
    double worst = 0.0;
    for (double f : {40.0, 100.0, 5000.0, 10000.0, 15000.0}) worst = std::fmax(worst, std::fabs(gain_at(f, 0.0f) - ref));
    s.check_le("response, iron off, 40 Hz-15 kHz re 1 kHz", worst, s.bar("response_db"), "dB");
    // Iron on: the primary inductance's roll-off, against the one-pole it is.
    const double ref_i = gain_at(1000.0, 100.0f);
    double dev = 0.0;
    for (double f : {20.0, 30.0, 60.0}) {
        const double want = 20.0 * std::log10(hp_mag(12.0, f, fs) * hp_mag(4.0, f, fs) / (hp_mag(12.0, 1000.0, fs) * hp_mag(4.0, 1000.0, fs)));
        dev = std::fmax(dev, std::fabs(gain_at(f, 100.0f) - ref_i - want));
    }
    s.check_le("iron on: low-frequency roll-off vs the one-pole it is (20, 30, 60 Hz)", dev, s.bar("iron.rolloff_db"), "dB");
}

void iron(Suite& s) {
    const double fs = 48000.0;
    auto thd_at = [&](double f, float iron) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"threshold", 0.0f}, {"time_constant", 3.0f}, {"iron", iron}}));
        p->prepare(fs, 128, 2);
        return measure::thdn_db(measure::render(*p, stereo_tone(fs, f, 0.0, 1.4))[0], 19200, fs, f);
    };
    // What the iron adds to the cell's own distortion, as a power
    // difference (the cell's cubic dominates at 1 kHz).
    auto added_db = [&](double f) {
        const double on = std::pow(10.0, thd_at(f, 100.0f) / 10.0), off = std::pow(10.0, thd_at(f, 0.0f) / 10.0);
        return 10.0 * std::log10(std::fmax(on - off, 1e-13));
    };
    const double lf = added_db(40.0), hf = added_db(1000.0);
    s.check("iron: the core saturates on low frequencies first (added THD at 40 Hz over 1 kHz, 0 dBFS)", lf - hf, s.bar("iron.lf_first_db"),
            lf - hf >= s.bar("iron.lf_first_db"), "dB", "40 Hz " + std::to_string(lf) + ", 1 kHz " + std::to_string(hf));
    s.check("iron: added THD at 40 Hz, 0 dBFS, within the stage's published-order range", lf, s.bar("iron.at_40hz_max_db"),
            lf <= s.bar("iron.at_40hz_max_db") && lf >= s.bar("iron.at_40hz_min_db"), "dB");
}

void link_and_hpf(Suite& s) {
    const double fs = 48000.0;
    // Left loud, right quiet.
    Chans x = stereo_tone(fs, 1000.0, -10.0, 0.8);
    const Chans q = stereo_tone(fs, 1000.0, -50.0, 0.8);
    x[1] = q[1];
    const auto n = x[0].size();
    const auto from = n - static_cast<std::size_t>(0.2 * fs);
    for (float link : {100.0f, 0.0f}) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"link", link}}));
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, x);
        const double gl = 20.0 * std::log10(measure::rms(y[0], from, n) / measure::rms(x[0], from, n));
        const double gr = 20.0 * std::log10(measure::rms(y[1], from, n) / measure::rms(x[1], from, n));
        if (link > 50.0) {
            s.check_le("stereo link 100 %: the quiet channel is reduced with the loud one, |difference|", std::fabs(gl - gr),
                       s.bar("link.linked_db"), "dB", "L " + std::to_string(gl) + " dB, R " + std::to_string(gr) + " dB");
        } else {
            s.check_le("stereo link 0 %: the quiet channel is left alone, reduction", -gr, s.bar("link.unlinked_db"), "dB",
                       "L " + std::to_string(gl) + " dB, R " + std::to_string(gr) + " dB");
        }
    }
    // Side-chain high-pass: a 50 Hz tone at -10 dBFS is reduced less with
    // the filter at 500 Hz.
    auto gr_of = [&](float on) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"time_constant", 3.0f}, {"sc_hpf_on", on}, {"sc_hpf_freq", 500.0f}}));
        p->prepare(fs, 128, 2);
        const Chans t = stereo_tone(fs, 50.0, -10.0, 3.0);
        const Chans y = measure::render(*p, t);
        const auto m = t[0].size();
        return -gain_db(y, t, m - static_cast<std::size_t>(1.0 * fs), m);
    };
    const double off = gr_of(0.0f), on = gr_of(1.0f);
    s.check("side-chain high-pass at 500 Hz: reduction of a 50 Hz tone falls (dB)", off - on, s.bar("hpf.fall_db"), off - on >= s.bar("hpf.fall_db"), "dB",
            "off " + std::to_string(off) + " dB, on " + std::to_string(on) + " dB");
}

void hot_input(Suite& s) {
    const double fs = 48000.0;
    bool finite = true;
    double worst = 0.0;
    for (double dbfs : {0.0, 12.0, 24.0}) {
        auto p = s.make();
        measure::apply(*p, with(base_config(), {{"input", 24.0f}, {"threshold", -50.0f}, {"quality", 1.0f}}));
        p->prepare(fs, 128, 2);
        Chans x = measure::noise(fs, 0.0, 0.3, 5u);
        for (auto& c : x)
            for (float& v : c) v = static_cast<float>(v * std::pow(10.0, dbfs / 20.0));
        const Chans y = measure::render(*p, x);
        for (const auto& c : y)
            for (float v : c) {
                finite &= std::isfinite(v);
                worst = std::fmax(worst, std::fabs(v));
            }
    }
    s.check("hot noise to +24 dBFS, input +24 dB: finite and bounded by the grid rail", worst, s.bar("hot.max_abs"),
            finite && worst <= s.bar("hot.max_abs"), "FS");
}

void mix(Suite& s) {
    auto p = s.make();
    measure::apply(*p, {{"mix", 0.0f}, {"threshold", -40.0f}, {"quality", 0.0f}});
    p->prepare(48000.0, 128, 2);
    const Chans x = measure::noise(48000.0, -6.0, 0.5, 3u);
    const Chans y = measure::render(*p, x);
    double worst = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < x[c].size(); ++i) worst = std::fmax(worst, std::fabs(y[c][i] - x[c][i]));
    s.check("mix 0 % equals the dry input", worst, 0.0, worst == 0.0, "abs");
}

void cpu_qualities(Suite& s, const Config& golden) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const Chans programme = measure::cpu_programme(fs, s.args().cpu_seconds, 0.0);
    const Chans hot = measure::cpu_programme(fs, s.args().cpu_seconds, 12.0);
    const json::Value* held = s.quality("cpu.held");
    for (int q = 0; q < 3; ++q) {
        bool assert_bar = false;
        if (held != nullptr && held->is_array())
            for (const json::Value& h : held->items()) assert_bar |= h.text() == kQualityIds[q];
        Config c = golden;
        c.push_back({"quality", static_cast<float>(q)});
        Config full = c;
        full.push_back({"iron", 100.0f});
        full.push_back({"threshold", -40.0f});
        full.push_back({"time_constant", 5.0f});
        measure::check_cpu_on(s, c, programme, std::string(", ") + kQualityIds[q], assert_bar);
        measure::check_cpu_on(s, full, hot, std::string(", ") + kQualityIds[q] + ", programme +12 dB, iron 100 %, deep reduction", assert_bar);
    }
}

}  // namespace

measure::Config varimu_golden_config() {
    // No quality: it reconfigures, so the golden renders at the default
    // (eco) and only the host fixture's reconfigure case moves it.
    return {{"input", 3.0f}, {"threshold", -24.0f}, {"time_constant", 4.0f}, {"sc_hpf_on", 1.0f},
            {"iron", 40.0f}, {"output", 1.0f},  {"mix", 90.0f}};
}

int varimu(const measure::Args& args) {
    Suite s("varimu", [] { return make_varimu(); }, args);
    static_curve(s);
    quality_agreement(s);
    releases(s);
    program_dependence(s);
    attacks(s);
    thd(s);
    aliasing(s);
    response(s);
    iron(s);
    link_and_hpf(s);
    hot_input(s);
    mix(s);
    const Config golden = varimu_golden_config();
    std::vector<std::pair<std::string, Config>> latency;
    for (int q = 0; q < 3; ++q) {
        if (measure::quick() && q == 1) continue;
        latency.push_back({kQualityIds[q], {{"quality", static_cast<float>(q)}}});
    }
    measure::check_latency(s, latency);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    Config denormal = golden;
    if (measure::quick()) denormal.push_back({"quality", 0.0f});
    measure::check_denormals(s, denormal);
    measure::check_zipper(s, {{"threshold", -30.0f}, {"time_constant", 2.0f}}, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
