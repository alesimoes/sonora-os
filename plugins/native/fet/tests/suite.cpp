// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FET limiter against its bars (quality.json). [M] is the reissue
// manual of the original unit, as in common/dsp fet.h; the research note
// (brain/research/native-fet-n2.md) has the citations and the numbers.
// - The circuit against ngspice on the same reduced circuit (the gate,
//   side chain, bias taps and rest point as behavioural sources) with
//   ngspice's own JFET and diode models (tests/spice/,
//   measure/spice_fet.h): the gain
//   reduction through tone bursts, at the base rate and at 4x and 8x of
//   44.1 to 96 kHz; and the processor in eco at 48 kHz against the circuit
//   it wraps, within 1e-7 FS (its float output).
// - The static curve per switch position: the ratio against the button's
//   label, the threshold rising with the ratio [M], the soft knee [M];
//   all buttons in, reported against [M]'s description. Standard against
//   high (8x) within a tenth of a dB.
// - Attack: t63 of the reduction on 10 kHz bursts, across the attack
//   control, against [M]'s 20 to 800 us; the stereo link doubling it [M].
// - Release: the release network against [M]'s 50 ms to 1.1 s; the gain
//   reduction's two-stage recovery reported.
// - THD against reduction: under [M]'s 0.5 % at its stated condition (the
//   slowest release), rising with the reduction at fast release; flat
//   response below threshold.
// - Aliasing at the nominal level, per quality; eco's level at fs/4 and
//   8 kHz against standard's, over the tone's phase.
// - The key input, the stereo link, hot input, and the common checks.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "sonora/dsp/fet.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/measure/spice_fet.h"
#include "sonora/native/fet.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
    char buf[240];
    std::snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}

double db(double x) { return 20.0 * std::log10(std::fmax(x, 1e-300)); }

json::Value jf(double v) { return json::Value::number_float(static_cast<float>(v)); }

std::string spice_dir(const Suite& s) { return s.args().golden_dir + "/../spice"; }

const char* const kQualityIds[] = {"eco", "standard", "high"};
const char* const kRatioNames[] = {"4:1", "8:1", "12:1", "20:1", "all buttons"};
const double kLabels[] = {4.0, 8.0, 12.0, 20.0};

// A processor set up for a measurement: input and output 0 dB and mix
// 100 %, so the output is the input times the divider's gain over its
// gain at rest, and the gain reduction can be read sample by sample.
std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    measure::apply(*p, {{"input", 0.0f}, {"output", 0.0f}, {"mix", 100.0f}});
    measure::apply(*p, c);
    p->prepare(fs, 128, channels);
    return p;
}

// Renders x (and the key) with the processor's latency taken out.
Chans aligned(Processor& p, const Chans& x, const Chans* key = nullptr) {
    const std::uint32_t lat = p.latency();
    Chans xp = x;
    for (auto& c : xp) c.resize(c.size() + lat, 0.0f);
    Chans kp;
    measure::RenderOptions opt;
    if (key != nullptr) {
        kp = *key;
        for (auto& c : kp) c.resize(c.size() + lat, 0.0f);
        opt.sidechain = &kp;
    }
    Chans y = measure::render(p, xp, {}, opt);
    for (auto& c : y) c.erase(c.begin(), c.begin() + lat);
    return y;
}

// The attack and release resistors for the controls' values (ms), as the
// processor maps them (fet.cpp, update_controls).
double r55_for(float ms) {
    const dsp::FetCircuit k;
    return static_cast<double>(ms) * 1e-3 / k.c22 - k.r54;
}
double r56_for(float ms) {
    const dsp::FetCircuit k;
    return static_cast<double>(ms) * 1e-3 / k.c22 - k.r57;
}

// A FetChannel at the processor's settings, from rest.
dsp::FetChannel channel(double rate, double ratio, float attack_ms, float release_ms) {
    dsp::FetChannel ch;
    ch.prepare(rate);
    ch.set_controls(ratio, r55_for(attack_ms), r56_for(release_ms));
    ch.reset();
    return ch;
}

// ---- SPICE -------------------------------------------------------------------

void spice(Suite& s) {
    measure::FetSpiceReference ref;
    std::string err;
    if (!measure::read_fet_spice_reference(spice_dir(s) + "/reference.json", ref, err)) {
        s.fail("SPICE reference: " + err);
        return;
    }
    const dsp::FetCircuit k;
    const dsp::JfetParams j;
    const dsp::DiodeParams d;
    const bool same = ref.circuit == measure::fet_circuit_json(k) && ref.devices == measure::fet_devices_json(j, d);
    s.check("SPICE reference made from the model's circuit and device values", same ? 1.0 : 0.0, 1.0, same, "bool",
            "regenerate with --spice-kind fet --spice-generate in a deliberate commit");
    s.info("SPICE reference: " + ref.ngspice, 0.0, "");
    const auto cases = measure::fet_spice_cases();
    if (cases.size() != ref.cases.size()) {
        s.fail("SPICE reference: case count differs from the harness's");
        return;
    }
    // At the fastest attack (R55 0) the attack network's R54 C27, 10 us,
    // is two steps of the standard quality at 44.1 kHz, and the steep first
    // microseconds of the attack carry the trapezoidal rule's error: those
    // cases are held to their own bar, the others to spice.gr_db.
    const double bar = s.bar("spice.gr_db"), bar_fast = s.bar("spice.fastest_attack_gr_db");
    // The circuit runs at the base rate times the quality's factor: held
    // at the default quality's (4x) rates, reported at eco's and high's.
    struct Rate {
        double fs;
        bool held;
        const char* quality;
    };
    const std::vector<Rate> rates =
        measure::quick() ? std::vector<Rate>{{192000.0, true, "standard"}}
                         : std::vector<Rate>{{44100.0, false, "eco"},       {48000.0, false, "eco"},
                                             {96000.0, false, "eco"},       {176400.0, true, "standard"},
                                             {192000.0, true, "standard"},  {384000.0, true, "standard"},
                                             {352800.0, false, "high"},     {768000.0, false, "high"}};
    json::Value rows = json::Value::array();
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        for (const Rate& rt : rates) {
            const std::vector<double> m = measure::fet_model_gr(c, rt.fs, ref.times[i], k, j, d);
            double worst = 0.0, worst_t = 0.0, peak = 0.0;
            for (std::size_t n = 0; n < m.size(); ++n) {
                const double dd = std::fabs(m[n] - ref.gr_db[i][n]);
                if (dd > worst) {
                    worst = dd;
                    worst_t = ref.times[i][n];
                }
                peak = std::fmax(peak, ref.gr_db[i][n]);
            }
            const std::string what = c.name + ", the circuit at " + hz(rt.fs) + " (" + rt.quality + "): max |GR - GR(SPICE)|";
            const std::string note = fmt("at %.5f s; deepest reduction %.2f dB", worst_t, peak);
            if (rt.held) s.check_le(what, worst, c.r55 > 0.0 ? bar : bar_fast, "dB", note);
            else s.info(what, worst, "dB", note);
            if (rt.fs == 192000.0 && rt.held) {
                json::Value row = json::Value::object();
                row.set("case", json::Value::string(c.name));
                json::Value t = json::Value::array(), a = json::Value::array(), b = json::Value::array();
                for (std::size_t n = 0; n < m.size(); ++n) {
                    t.push(jf(ref.times[i][n]));
                    a.push(jf(m[n]));
                    b.push(jf(ref.gr_db[i][n]));
                }
                row.set("t_s", t);
                row.set("model_gr_db", a);
                row.set("spice_gr_db", b);
                rows.push(row);
            }
        }
    }
    s.data().set("spice", rows);

    // The processor runs that circuit: eco (the circuit at the base rate),
    // mono, unkeyed, input and output 0 dB, mix 100, against FetChannel on
    // the first case's input, at the processor's attack and release.
    const auto& c = cases[0];
    const double fs = 48000.0;
    const float attack = 0.5f, release = 59.4f;
    auto proc = measuring(s, fs, 1, {{"ratio", static_cast<float>(c.ratio)}, {"attack", attack},
                                     {"release", release}, {"quality", 0.0f}});
    const auto len = static_cast<std::size_t>((c.quiet_s + c.hot_s + c.release_s) * fs);
    Chans x(1, std::vector<float>(len));
    for (std::size_t n = 0; n < len; ++n)
        x[0][n] = static_cast<float>(measure::fet_case_input(c, static_cast<double>(n) / fs));
    const Chans y = measure::render(*proc, x);
    dsp::FetChannel ch = channel(fs, c.ratio, attack, release);
    const double norm = 1.0 / (ch.rest_gain() * k.full_scale_v);
    double worst = 0.0;
    for (std::size_t n = 0; n < len; ++n) {
        const auto want = static_cast<float>(ch.tick(x[0][n] * k.full_scale_v) * norm);
        worst = std::fmax(worst, std::fabs(static_cast<double>(y[0][n]) - want));
    }
    s.check_le("eco processor against the circuit it wraps (" + c.name + " input, 48 kHz): max |difference|", worst,
               1e-7, "FS");
}

// ---- static curve --------------------------------------------------------------

// The steady gain reduction of a 1 kHz sine: the output's RMS over the
// last 0.5 s against the input's. Settled for 6 s: C9 brings the gate to
// the control voltage over about a second (fet.h).
double steady_gr(const Suite& s, double fs, float ratio, double level, float quality = 1.0f, double secs = 6.0) {
    auto p = measuring(s, fs, 1, {{"ratio", ratio}, {"quality", quality}});
    const Chans x = measure::sine(fs, 1000.0, level, secs, 1);
    const Chans y = aligned(*p, x);
    const std::size_t from = x[0].size() - static_cast<std::size_t>(0.5 * fs);
    return -db(measure::rms(y[0], from) / measure::rms(x[0], from));
}

void static_curve(Suite& s) {
    const double fs = 48000.0;
    const bool quick = measure::quick();
    const double step = quick ? 4.0 : 1.0;
    const double lo = -50.0, hi = 12.0;
    json::Value curves = json::Value::array();
    double prev_threshold = -1e9;
    bool rising = true;
    std::string thresholds;
    const double tol_low = s.bar("static.ratio_below"), tol_high = s.bar("static.ratio_above");
    for (int r = 0; r <= dsp::kFetAllButtons; ++r) {
        if (quick && (r == 1 || r == 2)) continue;
        std::vector<double> L, G;
        for (double l = lo; l <= hi + 1e-9; l += step) {
            L.push_back(l);
            G.push_back(steady_gr(s, fs, static_cast<float>(r), l));
        }
        // The ratio as the slope of input over output between neighbours,
        // at the points where the reduction is 3 to 15 dB.
        double peak_ratio = 0.0, at_gr = 0.0, threshold = 99.0;
        json::Value pts = json::Value::array();
        for (std::size_t i = 0; i < L.size(); ++i) {
            if (threshold > 90.0 && G[i] >= 1.0) threshold = L[i];
            double ratio = 0.0;
            if (i > 0 && i + 1 < L.size()) {
                const double dout = (L[i + 1] - G[i + 1]) - (L[i - 1] - G[i - 1]);
                ratio = dout > 0.0 ? (L[i + 1] - L[i - 1]) / dout : 1e9;
                if (G[i] >= 3.0 && G[i] <= 15.0 && ratio > peak_ratio) {
                    peak_ratio = ratio;
                    at_gr = G[i];
                }
            }
            json::Value pt = json::Value::array();
            pt.push(jf(L[i]));
            pt.push(jf(L[i] - G[i]));
            pt.push(jf(ratio));
            pts.push(pt);
        }
        json::Value c = json::Value::object();
        c.set("ratio", json::Value::string(kRatioNames[r]));
        c.set("in_out_slope", pts);
        curves.push(c);
        const std::string where = fmt("1 dB of reduction at %.0f dBFS (input 0 dB)", threshold);
        if (r < dsp::kFetAllButtons) {
            const double label = kLabels[r];
            const bool ok = peak_ratio >= label * (1.0 - tol_low) && peak_ratio <= label * (1.0 + tol_high);
            s.check(std::string("static curve, ") + kRatioNames[r] + ": steepest ratio from 3 to 15 dB of reduction",
                    peak_ratio, label, ok, ":1",
                    fmt("at %.1f dB of reduction; bar %.1f to %.1f against the label; ", at_gr,
                        label * (1.0 - tol_low), label * (1.0 + tol_high)) + where);
            rising &= threshold > prev_threshold;
            prev_threshold = threshold;
            thresholds += fmt("%.0f ", threshold);
        } else {
            s.info(std::string("static curve, all buttons: steepest ratio from 3 to 15 dB of reduction"), peak_ratio,
                   ":1", fmt("at %.1f dB of reduction; ", at_gr) + where +
                             "; [M]: 'between 12:1 and 20:1' (not met under our reading of the switch, research note)");
        }
    }
    s.check("threshold rises with the ratio ([M]: 'higher ratios also set the threshold higher')", rising ? 1.0 : 0.0,
            1.0, rising, "bool", thresholds + "dBFS");
    s.data().set("static_curves", curves);

    // The discretisation: standard (4x) against high (8x) through the knee
    // and into limiting, 4:1 and 20:1.
    double worst = 0.0;
    for (float r : {0.0f, 3.0f}) {
        for (double l : {-24.0, -12.0, 0.0}) {
            if (quick && l != -12.0) continue;
            worst = std::fmax(worst, std::fabs(steady_gr(s, fs, r, l, 1.0f) - steady_gr(s, fs, r, l, 2.0f)));
        }
    }
    s.check_le("static curve, standard (4x) against high (8x): max |difference|", worst, s.bar("static.quality_db"),
               "dB");
}

// ---- attack -------------------------------------------------------------------

struct Attack {
    double t63, t90, final_gr;
};

// A burst of `f` from -60 dBFS to `level`, on the circuit at 768 kHz (the
// high quality's rate at 96 kHz): at 192 kHz a 20 us attack is four
// samples and t63 read 15.6 or 20.8 us by the grid alone; here the
// crossing is interpolated between samples 1.3 us apart. The processor
// runs this circuit (spice() holds it to it). The reduction from the
// gate's state; t63 against the reduction at 0.3 s. Linked: a pair with
// the second channel silent, as two units joined through the stereo
// adapter.
Attack attack_of(double ratio, float attack_ms, double f, double level, bool linked = false) {
    const double rate = 768000.0;
    dsp::FetChannel a = channel(rate, ratio, attack_ms, 250.0f), b = channel(rate, ratio, attack_ms, 250.0f);
    const dsp::FetCircuit& k = a.circuit();
    const auto n0 = static_cast<std::size_t>(0.05 * rate), n1 = n0 + static_cast<std::size_t>(0.3 * rate);
    const double q = std::pow(10.0, -60.0 / 20.0) * k.full_scale_v, h = std::pow(10.0, level / 20.0) * k.full_scale_v;
    std::vector<double> gr(n1);
    for (std::size_t n = 0; n < n1; ++n) {
        // f 0: a DC step, which the rectifier sees at once, so the attack
        // is the network's alone (no wait for the burst's first peak).
        const double x = f > 0.0 ? (n >= n0 ? h : q) * std::sin(dsp::kTwoPi * f * static_cast<double>(n) / rate)
                                 : (n >= n0 ? h : 0.0);
        if (linked) {
            dsp::FetChannel::step_pair(a, b, x, 0.0, 1.0);
        } else {
            a.tick(x);
        }
        gr[n] = a.gain_reduction_db();
    }
    Attack r{-1.0, -1.0, gr[n1 - 1]};
    auto crossing = [&](double target) {
        for (std::size_t n = n0; n < n1; ++n) {
            if (gr[n] >= target) {
                const double prev = n > n0 ? gr[n - 1] : gr[n];
                const double u = gr[n] > prev ? (target - prev) / (gr[n] - prev) : 1.0;
                return (static_cast<double>(n - n0) - 1.0 + u) / rate;
            }
        }
        return -1.0;
    };
    r.t63 = crossing((1.0 - std::exp(-1.0)) * r.final_gr);
    r.t90 = crossing(0.9 * r.final_gr);
    return r;
}

void attack(Suite& s) {
    // [M]'s 20 and 800 us are the ends of the control. A feedback
    // limiter's attack depends on the step (the rectifier's drive against
    // what C22 must move), so the bars are at one stated step, held in
    // quality.json (attack.level_dbfs: a 10 kHz burst from -60 dBFS to
    // that level, 20:1, input 0 dB), and the dependence on the step is
    // reported below.
    const double level = s.bar("attack.level_dbfs");
    const double fastest_lo = s.bar("attack.fastest_min_us"), fastest_hi = s.bar("attack.fastest_max_us");
    const double slowest_lo = s.bar("attack.slowest_min_us"), slowest_hi = s.bar("attack.slowest_max_us");
    const float settings[] = {0.1034f, 0.3f, 1.0f, 3.0f, 5.6034f};
    json::Value rows = json::Value::array();
    double prev = 0.0;
    bool monotone = true;
    std::string all;
    Attack ends[2] = {};
    for (std::size_t i = 0; i < std::size(settings); ++i) {
        const Attack a = attack_of(3.0, settings[i], 10000.0, level);
        monotone &= a.t63 > prev;
        prev = a.t63;
        all += fmt("%.1f ", a.t63 * 1e6);
        if (i == 0) ends[0] = a;
        if (i + 1 == std::size(settings)) ends[1] = a;
        json::Value r = json::Value::array();
        r.push(jf(settings[i]));
        r.push(jf(a.t63 * 1e6));
        r.push(jf(a.t90 * 1e6));
        r.push(jf(a.final_gr));
        rows.push(r);
    }
    const std::string step = fmt("20:1, 10 kHz from -60 to %+.0f dBFS", level);
    s.check("attack, fastest (0.10 ms network): t63 of the reduction, " + step, ends[0].t63 * 1e6, fastest_hi,
            ends[0].t63 * 1e6 >= fastest_lo && ends[0].t63 * 1e6 <= fastest_hi, "us",
            fmt("%.1f dB at 0.3 s; bar %.0f-%.0f us ([M]: 20 us)", ends[0].final_gr, fastest_lo, fastest_hi));
    s.check("attack, slowest (5.6 ms network): t63 of the reduction, " + step, ends[1].t63 * 1e6, slowest_hi,
            ends[1].t63 * 1e6 >= slowest_lo && ends[1].t63 * 1e6 <= slowest_hi, "us",
            fmt("%.1f dB at 0.3 s; bar %.0f-%.0f us ([M]: 800 us)", ends[1].final_gr, slowest_lo, slowest_hi));
    s.check("attack lengthens monotonically with the control (0.10, 0.3, 1, 3, 5.6 ms)", monotone ? 1.0 : 0.0, 1.0,
            monotone, "bool", all + "us");
    s.data().set("attack_ms_t63us_t90us_gr", rows);

    // [M]: linked through the stereo adapter, "the fastest attack time is
    // doubled": one channel then charges both C22 nodes. Held on the
    // network in dsp_fet (C22's charging rate halves); on this burst the
    // reduction's t63 grows less, because at the fastest setting the
    // burst's peaks, not the network, set it. Reported.
    const Attack single = ends[0];
    const Attack linked = attack_of(3.0, 0.1034f, 10000.0, level, true);
    s.info("stereo link: fastest attack, linked over unlinked (one channel driven), " + step,
           linked.t63 / single.t63, "x",
           fmt("%.1f us linked, %.1f us alone ([M]: 'doubled', 40 us for 20)", linked.t63 * 1e6, single.t63 * 1e6));

    // How the attack depends on the step: reported.
    json::Value steps = json::Value::array();
    for (double l : {-10.0, -5.0, 0.0, 5.0, 10.0, 20.0}) {
        const Attack f = attack_of(3.0, 0.1034f, 10000.0, l), sl = attack_of(3.0, 5.6034f, 10000.0, l);
        const Attack lk = attack_of(3.0, 0.1034f, 10000.0, l, true);
        s.info(fmt("attack, 20:1, 10 kHz from -60 to %+.0f dBFS: t63 fastest, slowest, fastest linked", l),
               f.t63 * 1e6, "us",
               fmt("%.0f us, %.1f us linked; %.1f dB at 0.3 s", sl.t63 * 1e6, lk.t63 * 1e6, f.final_gr));
        json::Value r = json::Value::array();
        r.push(jf(l));
        r.push(jf(f.t63 * 1e6));
        r.push(jf(sl.t63 * 1e6));
        r.push(jf(lk.t63 * 1e6));
        r.push(jf(f.final_gr));
        steps.push(r);
    }
    s.data().set("attack_level_fastest_slowest_linked_us_gr", steps);
    for (double l : {-10.0, -5.0, 0.0, 10.0}) {
        const Attack f = attack_of(3.0, 0.1034f, 0.0, l), sl = attack_of(3.0, 5.6034f, 0.0, l);
        const Attack lk = attack_of(3.0, 0.1034f, 0.0, l, true);
        s.info(fmt("attack, 20:1, DC step from 0 to %+.0f dBFS: t63 fastest, slowest, fastest linked", l), f.t63 * 1e6,
               "us", fmt("%.0f us, %.2f us linked; %.1f dB at 0.3 s (C27 couples a step's first part at once)",
                         sl.t63 * 1e6, lk.t63 * 1e6, f.final_gr));
    }
}

// ---- release ------------------------------------------------------------------

struct Release {
    double vc_tau, gr0, t50, t90;
    std::vector<double> trace;  // GR every 10 ms from the drop
};

// 1 s of a 1 kHz tone at `level`, then -60 dBFS, on the circuit at 192 kHz.
// The control voltage's excess over rest falling to 1/e, and the
// reduction falling to half and a tenth of its value at the drop.
Release release_of(double ratio, float release_ms, double level) {
    const double rate = 192000.0;
    dsp::FetChannel ch = channel(rate, ratio, 0.5f, release_ms);
    const double fsv = ch.circuit().full_scale_v;
    const auto n1 = static_cast<std::size_t>(1.2 * rate), n2 = n1 + static_cast<std::size_t>(6.0 * rate);
    const auto n0 = static_cast<std::size_t>(0.2 * rate);
    double base = ch.control_v();
    Release r{-1.0, 0.0, -1.0, -1.0, {}};
    double top = 0.0;
    for (std::size_t n = 0; n < n2; ++n) {
        const double a = (n >= n0 && n < n1) ? std::pow(10.0, level / 20.0) : 1e-3;
        ch.tick(a * fsv * std::sin(dsp::kTwoPi * 1000.0 * static_cast<double>(n) / rate));
        if (n + 1 == n1) {
            top = ch.control_v() - base;
            r.gr0 = ch.gain_reduction_db();
        }
        if (n >= n1) {
            const double t = static_cast<double>(n - n1) / rate;
            const double g = ch.gain_reduction_db();
            if (r.vc_tau < 0.0 && ch.control_v() - base <= std::exp(-1.0) * top) r.vc_tau = t;
            if (r.t50 < 0.0 && g <= 0.5 * r.gr0) r.t50 = t;
            if (r.t90 < 0.0 && g <= 0.1 * r.gr0) r.t90 = t;
            if ((n - n1) % static_cast<std::size_t>(0.01 * rate) == 0) r.trace.push_back(g);
        }
    }
    return r;
}

void release(Suite& s) {
    // The release network, (R57 + R56) C22, is the control's value; [M]'s
    // range is its ends. Measured on the control voltage, which C9's path
    // (R6 + R7 to C9) also loads.
    const double lo = s.bar("release.network_min"), hi = s.bar("release.network_max");
    json::Value rows = json::Value::array();
    for (float ms : {59.4f, 250.0f, 1159.4f}) {
        const Release r = release_of(3.0, ms, 0.0);
        const double ratio = r.vc_tau * 1e3 / ms;
        s.check(fmt("release %.0f ms: the control voltage's 1/e time over the control's value", ms), ratio, hi,
                ratio >= lo && ratio <= hi, "x",
                fmt("%.1f ms; 20:1 from %.1f dB; bar %.2f-%.2f", r.vc_tau * 1e3, r.gr0, lo, hi));
        s.info(fmt("release %.0f ms: the reduction to half, and to a tenth (two stages: C22, then C9)", ms), r.t50 * 1e3,
               "ms", fmt("a tenth at %.0f ms; from %.1f dB", r.t90 * 1e3, r.gr0));
        json::Value row = json::Value::object();
        row.set("release_ms", jf(ms));
        row.set("vc_tau_ms", jf(r.vc_tau * 1e3));
        row.set("gr0_db", jf(r.gr0));
        row.set("t50_ms", jf(r.t50 * 1e3));
        row.set("t90_ms", jf(r.t90 * 1e3));
        json::Value tr = json::Value::array();
        for (double v : r.trace) tr.push(jf(v));
        row.set("gr_every_10ms", tr);
        rows.push(row);
    }
    s.data().set("release", rows);
    // [M]'s range against the control's: its ends from the circuit's values.
    const dsp::FetCircuit k;
    const double fast = k.r57 * k.c22, slow = (k.r57 + k.r56_max) * k.c22;
    const bool range = fast >= 0.05 * 0.8 && fast <= 0.05 * 1.25 && slow >= 1.1 * 0.8 && slow <= 1.1 * 1.25;
    s.check("release network's range from [M]'s values against [M]'s 50 ms to 1.1 s (within 20 %)", slow, 1.1, range,
            "s", fmt("%.1f ms to %.2f s", fast * 1e3, slow));
}

// ---- distortion, response, aliasing -------------------------------------------------

// The level for `gr` dB of steady reduction at 1 kHz (bisection on short
// renders of the circuit at 192 kHz, 4 s each).
double level_for_gr(double ratio, float release_ms, double gr) {
    double lo = -60.0, hi = 30.0;
    for (int it = 0; it < 24; ++it) {
        const double mid = 0.5 * (lo + hi);
        dsp::FetChannel ch = channel(48000.0, ratio, 0.5f, release_ms);
        const double a = std::pow(10.0, mid / 20.0) * ch.circuit().full_scale_v;
        double sx = 0.0, sy = 0.0;
        const int n = 48000 * 4, from = n - 24000;
        for (int i = 0; i < n; ++i) {
            const double x = a * std::sin(dsp::kTwoPi * 1000.0 * i / 48000.0);
            const double y = ch.tick(x);
            if (i >= from) {
                sx += x * x;
                sy += y * y;
            }
        }
        const double g = -db(std::sqrt(sy / sx) / ch.rest_gain());
        if (g < gr) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

void distortion(Suite& s) {
    const double fs = 48000.0;
    const double bar = s.bar("thd.max_pct_slowest");
    json::Value rows = json::Value::array();
    const std::vector<double> depths = measure::quick() ? std::vector<double>{3.0, 10.0, 20.0}
                                                        : std::vector<double>{1.0, 3.0, 6.0, 10.0, 15.0, 20.0};
    double worst_slow = 0.0;
    std::string worst_where;
    for (float ratio : {0.0f, 3.0f}) {
        for (float rel : {1159.4f, 59.4f}) {
            double prev = -1.0;
            bool rising = true;
            std::string all;
            for (double gr : depths) {
                const double level = level_for_gr(ratio, rel, gr);
                const std::vector<double> freqs =
                    rel > 1000.0 && gr == 10.0 ? std::vector<double>{50.0, 1000.0, 15000.0} : std::vector<double>{1000.0};
                for (double f : freqs) {
                    auto p = measuring(s, fs, 1, {{"ratio", ratio}, {"release", rel}, {"attack", 0.5f}});
                    const Chans x = measure::sine(fs, f, level, 6.0, 1);
                    const Chans y = aligned(*p, x);
                    const std::size_t from = static_cast<std::size_t>(5.0 * fs);
                    const double thd = 100.0 * std::pow(10.0, measure::thdn_db(y[0], from, fs, f) / 20.0);
                    if (rel > 1000.0 && thd > worst_slow) {
                        worst_slow = thd;
                        worst_where = std::string(kRatioNames[static_cast<int>(ratio)]) +
                                      fmt(", %.0f Hz, %.0f dB of reduction", f, gr);
                    }
                    if (f == 1000.0) {
                        rising &= thd > prev;
                        prev = thd;
                        all += fmt("%.3f ", thd);
                    }
                    json::Value r = json::Value::array();
                    r.push(jf(ratio));
                    r.push(jf(rel));
                    r.push(jf(f));
                    r.push(jf(gr));
                    r.push(jf(thd));
                    rows.push(r);
                }
            }
            const std::string what = std::string("THD+N at 1 kHz, ") + kRatioNames[static_cast<int>(ratio)] +
                                     fmt(", release %.0f ms", rel);
            if (rel < 100.0)
                s.check(what + ": rises with the reduction", rising ? 1.0 : 0.0, 1.0, rising, "bool", all + "%");
            else
                s.info(what + ", by reduction", prev, "%", all + "%");
        }
    }
    s.check_le("THD+N at the slowest release, 50 Hz to 15 kHz, up to 20 dB of reduction", worst_slow, bar, "%",
               "worst: " + worst_where + " ([M]: no greater than 0.5 % with limiting at the 1.1 s release)");
    s.data().set("thd_ratio_release_f_gr_pct", rows);

    // Below threshold the divider is resistive: flat. [M]: +-1 dB, 20 Hz to
    // 20 kHz (the transformers'); held tighter, the stage has no reactance.
    double flat = 0.0;
    for (double f : {20.0, 1000.0, 20000.0}) {
        auto p = measuring(s, fs, 1, {{"ratio", 3.0f}});
        const Chans x = measure::sine(fs, f, -60.0, 1.0, 1);
        const Chans y = aligned(*p, x);
        const std::size_t from = static_cast<std::size_t>(0.5 * fs);
        flat = std::fmax(flat, std::fabs(db(measure::rms(y[0], from) / measure::rms(x[0], from))));
    }
    s.check_le("response below threshold (-60 dBFS, 20:1), 20 Hz, 1 kHz, 20 kHz: max |gain|", flat,
               s.bar("response_db"), "dB");
}

// Non-harmonic energy in the band on steady tones; the frequencies are
// prime-ish so no alias lands on a harmonic. Nominal: the -18 dBFS
// alignment level (EBU R68) with the input at +6 dB, 4:1 at the default
// attack and release, about 6 dB of reduction; held to the quality's bar
// (§3.2: -90 dBc by default, -70 dBc in eco). The fastest attack and
// release, and all buttons, are reported.
void aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 7993.0, 10007.0, 14009.0, 19001.0};
    struct Level {
        const char* name;
        Config cfg;
    };
    const Level levels[] = {
        {"nominal (-18 dBFS, input +6 dB, 4:1)", {{"input", 6.0f}, {"ratio", 0.0f}}},
        {"20:1, input +18 dB, fastest attack and release",
         {{"input", 18.0f}, {"ratio", 3.0f}, {"attack", 0.1034f}, {"release", 59.4f}}},
        {"all buttons, input +12 dB, fastest attack and release",
         {{"input", 12.0f}, {"ratio", 4.0f}, {"attack", 0.1034f}, {"release", 59.4f}}},
    };
    const std::vector<double> rates =
        measure::quick() ? std::vector<double>{44100.0} : std::vector<double>{44100.0, 48000.0, 96000.0};
    const char* qnames[] = {"eco", "standard (4x)", "high (8x)"};
    const char* bars[] = {"aliasing.eco_dbc", "aliasing.standard_dbc", "aliasing.high_dbc"};
    json::Value rows = json::Value::array();
    for (int q = 0; q < 3; ++q) {
        for (double fs : rates) {
            for (std::size_t lv = 0; lv < std::size(levels); ++lv) {
                const Level& L = levels[lv];
                double worst = -400.0, worst_f = 0.0, gr = 0.0;
                for (double f0 : tones) {
                    if (f0 > 0.45 * fs) continue;
                    Config c = L.cfg;
                    c.push_back({"quality", static_cast<float>(q)});
                    auto p = measuring(s, fs, 1, c);
                    const Chans x = measure::sine(fs, f0, -18.0, 2.0, 1);
                    const Chans y = aligned(*p, x);
                    const std::size_t from = static_cast<std::size_t>(1.2 * fs);
                    const double a = measure::alias_dbc(y[0], from, fs, f0);
                    if (f0 == 997.0) {
                        float in = 0.0f;
                        for (const auto& kv : L.cfg)
                            if (std::string(kv.first) == "input") in = kv.second;
                        gr = in - db(measure::rms(y[0], from) / measure::rms(x[0], from));
                    }
                    if (a > worst) {
                        worst = a;
                        worst_f = f0;
                    }
                    json::Value r = json::Value::object();
                    r.set("quality", json::Value::integer(q));
                    r.set("rate", jf(fs));
                    r.set("tone", jf(f0));
                    r.set("level", json::Value::integer(static_cast<std::int64_t>(lv)));
                    r.set("alias_dbc", jf(a));
                    rows.push(r);
                }
                const std::string what = std::string("aliasing, ") + qnames[q] + ", " + hz(fs) + ", " + L.name +
                                         ", tones 1-19 kHz: worst non-harmonic";
                const std::string note = fmt("at %.0f Hz; %.1f dB of reduction at 1 kHz", worst_f, gr);
                if (lv == 0) s.check_le(what, worst, s.bar(bars[q]), "dBc", note);
                else s.info(what, worst, "dBc", note);
            }
        }
    }
    s.data().set("aliasing", rows);
}

// Eco's level at high frequencies (the lesson of PR #57's review: eco
// samples the rectifier's conduction a few times a period, so on a tone
// locked to the rate its reduction can depend on the tone's phase against
// the samples). Held: eco against standard over nine phases (0 to pi/2),
// at fs/4 and at 8 kHz, 20:1 at the default attack and release, from near
// the threshold into heavy limiting.
void eco_high_frequency(Suite& s) {
    struct Case {
        double fs, f;
        const char* bar;
    };
    const Case cases[] = {{48000.0, 12000.0, "eco_hf.fs4_db"},
                          {44100.0, 11025.0, "eco_hf.fs4_db"},
                          {48000.0, 8000.0, "eco_hf.f8k_db"}};
    const double secs = measure::quick() ? 3.0 : 5.0;
    json::Value rows = json::Value::array();
    for (const Case& c : cases) {
        const auto len = static_cast<std::size_t>(secs * c.fs);
        const std::size_t from = len - static_cast<std::size_t>(0.25 * c.fs);
        // 20:1 reduces by 1 dB near -10 dBFS: from the knee into about
        // 11 dB of limiting.
        for (double level : {-8.0, -4.0, 0.0}) {
            const double a = std::pow(10.0, level / 20.0);
            auto gr = [&](float quality, double phase) {
                Chans x(1, std::vector<float>(len));
                for (std::size_t n = 0; n < len; ++n)
                    x[0][n] = static_cast<float>(a * std::sin(dsp::kTwoPi * c.f * static_cast<double>(n) / c.fs + phase));
                auto p = measuring(s, c.fs, 1, {{"ratio", 3.0f}, {"quality", quality}});
                const Chans y = aligned(*p, x);
                return -db(measure::rms(y[0], from) / measure::rms(x[0], from));
            };
            const double ref = gr(1.0f, 0.0);
            double lo = 1e300, hi = -1e300;
            for (int i = 0; i <= 8; ++i) {
                const double g = gr(0.0f, i * dsp::kTwoPi / 32.0);
                lo = std::fmin(lo, g);
                hi = std::fmax(hi, g);
            }
            const double dev = std::fmax(hi - ref, ref - lo);
            const std::string what = fmt("eco, %.0f Hz at %.0f Hz, %.0f dBFS", c.f, c.fs, level) +
                                     ", 20:1: max |reduction - standard's| over 9 phases";
            s.check_le(what, dev, s.bar(c.bar), "dB", fmt("eco %.2f to %.2f dB, standard %.2f dB", lo, hi, ref));
            json::Value r = json::Value::array();
            r.push(jf(c.fs));
            r.push(jf(c.f));
            r.push(jf(level));
            r.push(jf(lo));
            r.push(jf(hi));
            r.push(jf(ref));
            rows.push(r);
        }
    }
    s.data().set("eco_hf_fs_f_level_lo_hi_standard", rows);
}

// ---- key, link, hot input --------------------------------------------------------------

void key_and_link(Suite& s) {
    const double fs = 48000.0;
    const Chans quiet = measure::sine(fs, 1000.0, -40.0, 4.0);
    const Chans loud = measure::sine(fs, 1000.0, -6.0, 4.0);
    const std::size_t from = static_cast<std::size_t>(3.5 * fs);
    auto gr_of = [&](const Chans& x, const Chans& y, std::size_t c) {
        return -db(measure::rms(y[c], from) / measure::rms(x[c], from));
    };
    {
        // External key: the side chain hears the key through a second
        // divider on the same gate, so a -40 dBFS input keyed by a -6 dBFS
        // signal is reduced as the -6 dBFS signal itself would be.
        auto p = measuring(s, fs, 2, {{"ratio", 0.0f}, {"external_sc", 1.0f}});
        const Chans y = aligned(*p, quiet, &loud);
        const double keyed = gr_of(quiet, y, 0);
        auto q = measuring(s, fs, 2, {{"ratio", 0.0f}});
        const Chans z = aligned(*q, quiet, &loud);
        const double unkeyed = gr_of(quiet, z, 0);
        auto r = measuring(s, fs, 2, {{"ratio", 0.0f}});
        const Chans w = aligned(*r, loud);
        const double own = gr_of(loud, w, 0);
        s.check("external key at -6 dBFS on a -40 dBFS input, 4:1: reduction", keyed, 3.0, keyed >= 3.0, "dB",
                fmt("key off: %.3f dB", unkeyed));
        s.check_le("external key: |reduction - the key's own reduction as the input|", std::fabs(keyed - own), 0.05,
                   "dB", fmt("the -6 dBFS signal itself: %.2f dB", own));
        s.check_le("external key off, the same input and key: reduction", unkeyed, 0.1, "dB");
    }
    {
        // Linked: the quiet channel follows the loud one.
        Chans x = loud;
        x[1] = quiet[1];
        auto p = measuring(s, fs, 2, {{"ratio", 3.0f}, {"link", 1.0f}});
        const Chans y = aligned(*p, x);
        auto q = measuring(s, fs, 2, {{"ratio", 3.0f}, {"link", 0.0f}});
        const Chans z = aligned(*q, x);
        const double d = std::fabs(gr_of(x, y, 0) - gr_of(x, y, 1));
        s.check_le("stereo link on, -6 and -40 dBFS: |reduction left - right|", d, 0.01, "dB",
                   fmt("%.2f dB on both", gr_of(x, y, 0)));
        s.check_le("stereo link off: reduction of the -40 dBFS channel", gr_of(x, z, 1), 0.1, "dB",
                   fmt("the -6 dBFS channel %.2f dB", gr_of(x, z, 0)));
    }
}

// Controls set after prepare() and before the first block (as a host
// restoring state does) arrive as jumps, the smoother settled at the new
// value; they must reach the circuit as they would set before prepare().
// A version that updated the circuit only while a smoother moved kept the
// old controls there, which the host renders caught (0.078 FS).
void controls_after_prepare(Suite& s) {
    const double fs = 48000.0;
    const Config cfg = {{"ratio", 3.0f}, {"attack", 3.0f}, {"release", 1000.0f}, {"input", 12.0f}};
    const Chans x = measure::sine(fs, 1000.0, -12.0, 1.0);
    auto before = s.make();
    measure::apply(*before, cfg);
    before->prepare(fs, 128, 2);
    const Chans a = measure::render(*before, x);
    auto after = s.make();
    after->prepare(fs, 128, 2);
    measure::apply(*after, cfg);
    const Chans b = measure::render(*after, x);
    double d = 0.0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t n = 0; n < a[c].size(); ++n) d = std::fmax(d, std::fabs(a[c][n] - b[c][n]));
    s.check_le("ratio, attack, release and input set after prepare, before the first block: |difference| from "
               "set before",
               d, 1e-6, "FS");
}

// Squares at 0, +12 and +24 dBFS with the input at +24 dB: the output
// finite, the reduction no deeper than the gate's forward limit allows,
// and the worst block (minimum of three runs, so one preemption does not
// count) no more than hot_block_ratio times the median block on a quiet
// square (-60 dBFS) in the same configuration (the bar of the tube's and
// the opto's reviews): the Newton loops' cost must not grow with the drive.
// The block is also held inside the period in an uninstrumented build, and
// reported under a sanitizer (Suite::check_time_le), as the tube's and the
// opto's are. The shortest period: at a quality a 128-frame block is 128 x
// factor circuit steps whatever the base rate, so the 48 kHz block must fit
// 128 frames at 192 kHz, 667 us (RT re-audit of #74, non-blocking 1: B1's
// regression showed only at the high rates).
// The Newton loops' cost stays flat in the drive, up to the processor's
// input ceiling: ProcessorBase accepts up to 1e6 FS, so with Input at +24 dB
// the circuit sees about 2e5 V at R5. Hot squares at +48 dB (3.2 V, the
// hottest the other checks drive) and at that ceiling, through the circuit
// at each quality's rate (48 kHz times 1, 4, 8), every switch position at
// the fastest attack and release, unlinked and linked: no step reaches the
// cap (FetChannel counts them rather than committing them silently), and
// the iterations a step, at most and on average, stay under
// newton.max_iterations and newton.mean_iterations. RT audit of #74, B1:
// before the fix the count grew with the log of the drive, about 16 a step
// at the ceiling, and high (8x) missed the period at 96 and 192 kHz.
void newton_cap(Suite& s) {
    const dsp::FetCircuit k;
    const double in24 = std::pow(10.0, 24.0 / 20.0);
    struct Drive {
        const char* name;
        double volts;
    };
    const Drive drives[] = {{"+48 dB (3.2 V at R5)", std::pow(10.0, 48.0 / 20.0) * k.full_scale_v},
                            {"999999 FS at Input +24 dB (the ceiling)", 999999.0 * in24 * k.full_scale_v}};
    const double max_bar = s.bar("newton.max_iterations"), mean_bar = s.bar("newton.mean_iterations");
    for (const Drive& d : drives) {
        std::uint64_t hits = 0, steps = 0, total = 0;
        int most = 0;
        for (double rate : {48000.0, 192000.0, 384000.0}) {
            for (int pos = 0; pos <= dsp::kFetAllButtons; ++pos) {
                for (double g : {0.0, 1.0}) {
                    dsp::FetChannel a = channel(rate, pos, 0.1034f, 59.4f), b = channel(rate, pos, 0.1034f, 59.4f);
                    const auto n = static_cast<int>(0.05 * rate);
                    for (int i = 0; i < n; ++i) {
                        const double x = std::sin(dsp::kTwoPi * 997.0 * i / rate) >= 0.0 ? d.volts : -d.volts;
                        dsp::FetChannel::step_pair(a, b, x, -x, g);
                        most = std::max(most, std::max(a.last_iterations(), b.last_iterations()));
                        total += static_cast<std::uint64_t>(a.last_iterations());
                        ++steps;
                    }
                    hits += a.cap_hits() + b.cap_hits();
                }
            }
        }
        const std::string at = std::string(", hot squares at ") + d.name + ", every quality's rate, position and link";
        s.check("Newton loops: steps that reached the cap" + at, static_cast<double>(hits), 0.0, hits == 0, "steps");
        s.check_le("Newton loops: most iterations a step" + at, most, max_bar, "iterations", "the cap is 60");
        s.check_le("Newton loops: mean iterations a step" + at, static_cast<double>(total) / static_cast<double>(steps),
                   mean_bar, "iterations");
    }
}

void hot_input(Suite& s) {
    const double fs = 48000.0;
    const Config configs[] = {{{"ratio", 3.0f}, {"quality", 1.0f}},
                              {{"ratio", 4.0f}, {"quality", 1.0f}, {"attack", 0.1034f}, {"release", 59.4f}},
                              {{"ratio", 0.0f}, {"quality", 0.0f}},
                              {{"ratio", 3.0f}, {"quality", 2.0f}},
                              {{"ratio", 3.0f}, {"quality", 1.0f}, {"link", 0.0f}}};
    const char* names[] = {"standard, 20:1", "standard, all buttons, fastest", "eco, 4:1", "high, 20:1",
                           "standard, link off, channels in anti-phase"};
    const double ratio_bar = s.bar("hot_block_ratio");
    const dsp::FetCircuit k;
    const dsp::JfetParams j;
    const double deepest_bar = 20.0 * std::log10(std::pow(10.0, -k.rest_loss_db / 20.0) *
                                                 (1.0 + k.r5 * 2.0 * j.beta * (k.gate_max_v - j.vto)));
    // An amplitude in FS (the processor's ceiling is 999999, under
    // ProcessorBase's 1e6).
    auto square = [&](double a, bool anti) {
        Chans x(2, std::vector<float>(static_cast<std::size_t>(1.0 * fs)));
        for (std::size_t n = 0; n < x[0].size(); ++n) {
            const float v =
                static_cast<float>(std::sin(dsp::kTwoPi * 997.0 * static_cast<double>(n) / fs) >= 0.0 ? a : -a);
            x[0][n] = v;
            x[1][n] = anti ? -v : v;
        }
        return x;
    };
    auto run_once = [&](const Config& cfg, const Chans& x, std::vector<double>& fastest, bool first) {
        auto p = s.make();
        measure::apply(*p, {{"input", 24.0f}, {"output", 0.0f}, {"mix", 100.0f}});
        measure::apply(*p, cfg);
        p->prepare(fs, 128, 2);
        std::vector<double> us;
        measure::RenderOptions opt;
        opt.block_us = &us;
        Chans xp = x;
        for (auto& ch : xp) ch.resize(ch.size() + p->latency(), ch.back());
        Chans y = measure::render(*p, xp, {}, opt);
        for (auto& ch : y) ch.erase(ch.begin(), ch.begin() + p->latency());
        if (first) fastest = us;
        for (std::size_t n = 0; n < us.size(); ++n) fastest[n] = std::fmin(fastest[n], us[n]);
        return y;
    };
    const double in_gain = std::pow(10.0, 24.0 / 20.0);
    for (std::size_t ci = 0; ci < std::size(configs); ++ci) {
        const bool anti = ci + 1 == std::size(configs);
        const Chans quiet = square(1e-3, anti);
        // 0, +12, +24 dBFS, and the input ceiling (999999 FS, about
        // +120 dBFS: RT audit of #74, B1, where high missed the period).
        for (double amp : {1.0, std::pow(10.0, 12.0 / 20.0), std::pow(10.0, 24.0 / 20.0), 999999.0}) {
            const double dbfs = 20.0 * std::log10(amp);
            const bool ceiling = amp > 1e5;
            const Chans x = square(amp, anti);
            std::vector<double> worst, calm;
            double deepest = 0.0;
            bool finite = true;
            for (int run = 0; run < 3; ++run) {
                run_once(configs[ci], quiet, calm, run == 0);
                const Chans y = run_once(configs[ci], x, worst, run == 0);
                for (std::size_t c = 0; c < 2; ++c) {
                    for (std::size_t n = static_cast<std::size_t>(fs / 2); n < y[c].size(); ++n) {
                        finite &= std::isfinite(y[c][n]);
                        deepest = std::fmax(deepest, -db(std::fabs(y[c][n] / (x[c][n] * in_gain))));
                    }
                }
            }
            const double w = *std::max_element(worst.begin() + 1, worst.end());
            std::vector<double> sorted(calm.begin() + 1, calm.end());
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            const std::string what = std::string(names[ci]) + fmt(", %+.0f dBFS square, input +24 dB", dbfs);
            s.check(what + ": output finite", finite ? 1.0 : 0.0, 1.0, finite, "bool");
            // At the ceiling the input is bounded at R5 (fet.h), so the
            // output against the input is that bound, not a reduction.
            if (!ceiling)
                s.check_le(what + ": deepest reduction, second half", deepest, deepest_bar + 0.5, "dB",
                           fmt("the gate's forward limit allows %.1f dB", deepest_bar));
            s.check_le(what + ": worst block over the quiet median, minimum of 3 runs", w / median, ratio_bar, "x",
                       fmt("worst %.1f us; median %.1f us on a -60 dBFS square", w, median));
            s.check_time_le(what + ": worst block, minimum of 3 runs", w, 667.0, "us",
                            "the 128-frame period at 192 kHz: block cost independent of the base rate");
        }
    }
}

// Class A (§3.5) per quality, on the programme signal at the golden
// settings and on a worst case: the programme raised 12 dB at 20:1 with
// the fastest attack and release, the rectifier conducting on every peak.
// Which qualities are held is quality.json's cpu.held; the others are
// reported.
void cpu_qualities(Suite& s, const Config& golden) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const Chans programme = measure::cpu_programme(fs, s.args().cpu_seconds, 0.0);
    const Chans hot = measure::cpu_programme(fs, s.args().cpu_seconds, 12.0);
    const char* names[] = {", eco", ", standard (4x)", ", high (8x)"};
    const json::Value* held = s.quality("cpu.held");
    for (int q = 0; q < 3; ++q) {
        bool assert_bar = false;
        if (held != nullptr && held->is_array())
            for (const json::Value& h : held->items()) assert_bar |= h.text() == kQualityIds[q];
        Config c = golden;
        c.push_back({"quality", static_cast<float>(q)});
        Config full = c;
        full.push_back({"ratio", 3.0f});
        full.push_back({"attack", 0.1034f});
        full.push_back({"release", 59.4f});
        measure::check_cpu_on(s, c, programme, names[q], assert_bar);
        measure::check_cpu_on(s, full, hot, std::string(names[q]) + ", programme +12 dB, 20:1, fastest", assert_bar);
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture).
measure::Config fet_golden_config() {
    return {{"input", 12.0f}, {"output", -3.0f}, {"attack", 0.3f}, {"release", 150.0f}, {"ratio", 1.0f}, {"mix", 90.0f}};
}

int fet(const measure::Args& args) {
    Suite s("fet", [] { return make_fet(); }, args);
    spice(s);
    static_curve(s);
    attack(s);
    release(s);
    distortion(s);
    aliasing(s);
    eco_high_frequency(s);
    key_and_link(s);
    controls_after_prepare(s);
    hot_input(s);
    newton_cap(s);
    const Config golden = fet_golden_config();
    // check_common() without its CPU check, which holds the golden
    // (standard) quality to the class bar: cpu_qualities() instead.
    std::vector<std::pair<std::string, Config>> latency;
    for (int q = 0; q < 3; ++q) {
        const float qf = static_cast<float>(q);
        const std::string n = kQualityIds[q];
        latency.push_back({n + ", 4:1", {{"quality", qf}}});
        latency.push_back({n + ", all buttons, fastest", {{"quality", qf}, {"ratio", 4.0f}, {"attack", 0.1034f}}});
        latency.push_back({n + ", external key", {{"quality", qf}, {"external_sc", 1.0f}}});
    }
    measure::check_latency(s, latency);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, golden, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
