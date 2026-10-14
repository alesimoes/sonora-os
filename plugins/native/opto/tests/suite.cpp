// SPDX-License-Identifier: GPL-3.0-or-later
//
// The opto against its bars (quality.json). [M] is the reissue manual of
// the original unit, as in common/dsp opto.h; the research note
// (brain/research/native-opto-n2.md) has the citations and the numbers.
// - The circuit against ngspice on the same equations (tests/spice/,
//   measure/spice_opto.h): the gain reduction through tone bursts, at
//   44.1, 48 and 96 kHz; and the processor bit for bit against the
//   circuit it wraps.
// - The static curve against the model's steady state, solved here from
//   the circuit's equations rather than rendered; the ratio in compress
//   ("gentle ... a low compression ratio" [M]) and in limit ("a higher
//   compression ratio" [M]).
// - Release: [M]'s figures, "approximately 0.06 seconds for 50% release,
//   0.5 to 5 seconds for complete release depending upon the amount of
//   previous reduction", after 1, 5 and 20 s of reduction and at three
//   depths; the release lengthening with both (the cell's memory).
// - Attack: "very fast" [M], and faster on a larger overshoot.
// - THD against level at 1 kHz against [M]'s 0.35 %; low-frequency
//   distortion reported; flat response below threshold ([M]: +-0.1 dB,
//   30 Hz to 15 kHz).
// - Aliasing at the nominal level, with no oversampling.
// - The key input, the stereo link, hot input, and the common checks.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/dsp/opto.h"
#include "sonora/measure/measure.h"
#include "sonora/measure/spice_opto.h"
#include "sonora/native/opto.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

double db(double x) { return 20.0 * std::log10(std::fmax(x, 1e-300)); }

json::Value jf(double v) { return json::Value::number_float(static_cast<float>(v)); }

std::string spice_dir(const Suite& s) { return s.args().golden_dir + "/../spice"; }

// A processor set up for a measurement: gain 0 dB and mix 100 %, so the
// output is the input times the attenuator's gain and the gain reduction
// can be read sample by sample.
std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    measure::apply(*p, {{"gain", 0.0f}, {"mix", 100.0f}});
    measure::apply(*p, c);
    p->prepare(fs, 128, channels);
    return p;
}

// Renders x (and the key) with the processor's latency taken out: the
// output sample n is the one input sample n produced.
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

// A 1 kHz (or f) tone whose level steps between segments, on whole
// periods so each step falls on a zero crossing. amp[n] is its amplitude.
struct Burst {
    Chans x;
    std::vector<float> amp;
    std::vector<std::size_t> edges;  // the first sample of each segment
};

Burst burst(double fs, double f, const std::vector<std::pair<double, double>>& segs, std::uint32_t channels = 1) {
    Burst b;
    std::size_t total = 0;
    for (const auto& sg : segs) {
        b.edges.push_back(total);
        // Whole periods.
        total += static_cast<std::size_t>(std::llround(std::round(sg.first * f) / f * fs));
    }
    b.x.assign(channels, std::vector<float>(total));
    b.amp.assign(total, 0.0f);
    std::size_t seg = 0;
    for (std::size_t n = 0; n < total; ++n) {
        while (seg + 1 < b.edges.size() && n >= b.edges[seg + 1]) ++seg;
        const double a = std::pow(10.0, segs[seg].second / 20.0);
        b.amp[n] = static_cast<float>(a);
        const float v = static_cast<float>(a * std::sin(dsp::kTwoPi * f * static_cast<double>(n) / fs));
        for (auto& ch : b.x) ch[n] = v;
    }
    return b;
}

// Gain reduction in dB at each sample, read as |x / y| where the input is
// at least half its amplitude, held between.
std::vector<double> gr_trace(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& amp) {
    std::vector<double> g(x.size(), 0.0);
    double last = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        if (std::fabs(x[n]) >= 0.5f * amp[n] && x[n] != 0.0f) last = -db(std::fabs(static_cast<double>(y[n]) / x[n]));
        g[n] = last;
    }
    return g;
}

// ---- the model's steady state, from the equations ---------------------------

struct Setting {
    const char* name;
    float limit, peak, emphasis;
};

// The output level for a steady sine of `level_dbfs` at f: the carriers n
// at which the cell's steady state under the cycle-averaged light of the
// side chain (driven by the tap, which depends on n) equals n. The
// generation falls as n rises, so the root is unique; bisection in log n.
double steady_out_db(const Setting& st, double level_dbfs, double f, double* gr_out = nullptr) {
    const dsp::OptoCircuit k;
    const dsp::OptoCellParams p;
    dsp::OptoCell cell;
    cell.prepare(48000.0, p);
    const double share = dsp::peak_reduction_share(st.peak * 0.01);
    const double sc = k.r2 / (k.r30 + k.r2) * k.amp_gain * share *
                      dsp::OptoChannel::sidechain_analog_mag(k, st.emphasis * 0.01 * k.r37_max, f);
    const double vpk = k.full_scale_v * std::pow(10.0, level_dbfs / 20.0);
    constexpr int kPoints = 4096;
    auto residual = [&](double n) {
        double g, tap;
        dsp::OptoChannel::divider(k, st.limit, p.dark_conductance + p.unit_conductance * n, p.dark_conductance, g, tap);
        double acc = 0.0;
        for (int i = 0; i < kPoints; ++i) {
            const double v = sc * tap * vpk * std::sin(dsp::kTwoPi * (i + 0.5) / kPoints);
            acc += cell.light(dsp::clamp(v, -k.drive_limit_v, k.drive_limit_v));
        }
        return dsp::OptoCell::steady_carriers(p.el_gain * acc / kPoints, p) - n;
    };
    double lo = 1e-12, hi = 100.0;
    if (residual(lo) <= 0.0) lo = 0.0;
    for (int it = 0; it < 200 && lo > 0.0; ++it) {
        const double mid = std::sqrt(lo * hi);
        if (residual(mid) > 0.0) lo = mid;
        else hi = mid;
    }
    const double n = lo > 0.0 ? std::sqrt(lo * hi) : 0.0;
    double g, tap;
    dsp::OptoChannel::divider(k, st.limit, p.dark_conductance + p.unit_conductance * n, p.dark_conductance, g, tap);
    if (gr_out != nullptr) *gr_out = -db(g);
    return level_dbfs + db(g);
}

// The level at which the steady reduction is `gr_db`.
double level_for_gr(const Setting& st, double gr_db, double f) {
    double lo = -80.0, hi = 30.0;
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        double g = 0.0;
        steady_out_db(st, mid, f, &g);
        if (g < gr_db) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

const char* const kQualityIds[] = {"eco", "standard", "high"};

// ---- SPICE -------------------------------------------------------------------

void spice(Suite& s) {
    measure::OptoSpiceReference ref;
    std::string err;
    if (!measure::read_opto_spice_reference(spice_dir(s) + "/reference.json", ref, err)) {
        s.fail("SPICE reference: " + err);
        return;
    }
    const dsp::OptoCircuit k;
    const dsp::OptoCellParams p;
    const bool same = ref.circuit == measure::opto_circuit_json(k) && ref.cell == measure::opto_cell_json(p);
    s.check("SPICE reference made from the model's circuit and cell values", same ? 1.0 : 0.0, 1.0, same, "bool",
            "regenerate with --spice-kind opto --spice-generate in a deliberate commit");
    s.info("SPICE reference: " + ref.ngspice, 0.0, "");
    const auto cases = measure::opto_spice_cases();
    if (cases.size() != ref.cases.size()) {
        s.fail("SPICE reference: case count differs from the harness's");
        return;
    }
    const double bar = s.bar("spice.gr_db");
    // The cell runs at the base rate times the quality's factor: held at
    // the default quality's (4x) rates, reported at eco's and high's.
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
            const double fs = rt.fs;
            const std::vector<double> m = measure::opto_model_gr(c, fs, ref.times[i], k, p);
            double worst = 0.0, worst_t = 0.0, peak = 0.0;
            for (std::size_t j = 0; j < m.size(); ++j) {
                const double d = std::fabs(m[j] - ref.gr_db[i][j]);
                if (d > worst) {
                    worst = d;
                    worst_t = ref.times[i][j];
                }
                peak = std::fmax(peak, ref.gr_db[i][j]);
            }
            const std::string what = c.name + ", the cell at " + hz(fs) + " (" + rt.quality + "): max |GR - GR(SPICE)|";
            const std::string note = fmt("at %.4f s; deepest reduction %.2f dB", worst_t, peak);
            if (rt.held) s.check_le(what, worst, bar, "dB", note);
            else s.info(what, worst, "dB", note);
            if (fs == 192000.0 && rt.held) {
                json::Value row = json::Value::object();
                row.set("case", json::Value::string(c.name));
                json::Value t = json::Value::array(), a = json::Value::array(), b = json::Value::array();
                for (std::size_t j = 0; j < m.size(); ++j) {
                    t.push(jf(ref.times[i][j]));
                    a.push(jf(m[j]));
                    b.push(jf(ref.gr_db[i][j]));
                }
                row.set("t_s", t);
                row.set("model_gr_db", a);
                row.set("spice_gr_db", b);
                rows.push(row);
            }
        }
    }
    s.data().set("spice", rows);

    // The processor runs that circuit: eco (the cell at the base rate),
    // mono, unlinked, unkeyed, gain 0, mix 100, against OptoChannel on the
    // first case's input.
    const auto& c = cases[0];
    const double fs = 48000.0;
    auto proc = measuring(s, fs, 1, {{"peak_reduction", static_cast<float>(c.peak)}, {"quality", 0.0f}});
    const auto len = static_cast<std::size_t>((c.quiet_s + c.hot_s + c.release_s) * fs);
    Chans x(1, std::vector<float>(len));
    for (std::size_t n = 0; n < len; ++n)
        x[0][n] = static_cast<float>(measure::opto_case_input(c, static_cast<double>(n) / fs));
    const Chans y = measure::render(*proc, x);
    dsp::OptoChannel ch;
    ch.prepare(fs);
    ch.set_peak_share(dsp::peak_reduction_share(c.peak * 0.01));
    double worst = 0.0;
    for (std::size_t n = 0; n < len; ++n)
        worst = std::fmax(worst, std::fabs(static_cast<double>(y[0][n]) - static_cast<float>(ch.tick(x[0][n]))));
    s.check_le("eco processor against the circuit it wraps (" + c.name + ", 48 kHz): max |difference|", worst, 1e-7,
               "FS");
}

// ---- static curve --------------------------------------------------------------

const Setting kCompress50{"compress, peak reduction 50", 0.0f, 50.0f, 0.0f};
const Setting kCompress75{"compress, peak reduction 75", 0.0f, 75.0f, 0.0f};
const Setting kCompress100{"compress, peak reduction 100", 0.0f, 100.0f, 0.0f};
const Setting kLimit75{"limit, peak reduction 75", 1.0f, 75.0f, 0.0f};
const Setting kLimit100{"limit, peak reduction 100", 1.0f, 100.0f, 0.0f};
const Setting kEmph100{"compress, peak reduction 100, emphasis 100", 0.0f, 100.0f, 100.0f};

// Input over output slope at the level where the steady reduction is gr_db.
double ratio_at(const Setting& st, double gr_db) {
    const double l = level_for_gr(st, gr_db, 1000.0);
    const double d = 0.5;
    return 2.0 * d / (steady_out_db(st, l + d, 1000.0) - steady_out_db(st, l - d, 1000.0));
}

void static_curve(Suite& s) {
    const double fs = 48000.0;
    const bool quick = measure::quick();
    const double secs = quick ? 12.0 : 30.0;
    const double step = quick ? 6.0 : 2.0;
    const double bar = s.bar("static.curve_db");
    json::Value curves = json::Value::array();
    const std::vector<Setting> settings = quick ? std::vector<Setting>{kCompress100, kLimit100}
                                                : std::vector<Setting>{kCompress50, kCompress75, kCompress100,
                                                                       kLimit75, kLimit100, kEmph100};
    for (const Setting& st : settings) {
        double worst = 0.0, worst_at = 0.0, deepest = 0.0;
        json::Value pts = json::Value::array();
        for (double level = -50.0; level <= 6.0 + 1e-9; level += step) {
            auto p = measuring(s, fs, 1,
                               {{"peak_reduction", st.peak}, {"mode", st.limit}, {"emphasis", st.emphasis}});
            const Chans x = measure::sine(fs, 1000.0, level, secs, 1);
            const Chans y = aligned(*p, x);
            const std::size_t from = x[0].size() - static_cast<std::size_t>(0.5 * fs);
            const double out = level + db(measure::rms(y[0], from) / measure::rms(x[0], from));
            double gr = 0.0;
            const double want = steady_out_db(st, level, 1000.0, &gr);
            if (std::fabs(out - want) > worst) {
                worst = std::fabs(out - want);
                worst_at = level;
            }
            deepest = std::fmax(deepest, gr);
            json::Value pt = json::Value::array();
            pt.push(jf(level));
            pt.push(jf(out));
            pt.push(jf(want));
            pts.push(pt);
        }
        s.check_le(std::string("static curve, ") + st.name + ", 1 kHz: max |out - model steady state|", worst, bar,
                   "dB", fmt("at %.0f dBFS in; deepest reduction %.1f dB", worst_at, deepest));
        json::Value c = json::Value::object();
        c.set("setting", json::Value::string(st.name));
        c.set("in_out_model_db", pts);
        curves.push(c);
    }
    s.data().set("static_curves", curves);

    // The ratio, from the model's steady state (which the curves above hold
    // the processor to): compress "a low compression ratio", limit "a
    // higher" one [M]. At 3, 6, 10 and 15 dB of reduction.
    const double max_ratio = s.bar("static.compress_max_ratio");
    json::Value ratios = json::Value::array();
    for (double peak : {75.0f, 100.0f}) {
        const Setting comp{"compress", 0.0f, static_cast<float>(peak), 0.0f};
        const Setting lim{"limit", 1.0f, static_cast<float>(peak), 0.0f};
        for (double gr : {3.0, 6.0, 10.0, 15.0}) {
            const double rc = ratio_at(comp, gr), rl = ratio_at(lim, gr);
            const std::string at = fmt("peak reduction %.0f, %.0f dB of reduction", peak, gr);
            s.check_le("compress ratio, " + at, rc, max_ratio, ":1");
            // Past infinity the limit position over-compresses (the output
            // falls as the input rises): a negative slope, a ratio "above"
            // any finite one.
            const bool higher = rl < 0.0 || rl > rc;
            s.check("limit ratio above compress, " + at, rl, rc, higher, ":1",
                    rl < 0.0 ? "over-compressing: the output falls as the input rises" : "");
            json::Value r = json::Value::array();
            r.push(jf(peak));
            r.push(jf(gr));
            r.push(jf(rc));
            r.push(jf(rl));
            ratios.push(r);
        }
    }
    s.data().set("ratios_peak_gr_compress_limit", ratios);
}

// ---- release and memory ---------------------------------------------------------

struct Release {
    double gr0, t50, t90;
    std::vector<double> trace;  // GR every 5 ms from the release
};

// `hold` seconds at the level for `depth` dB of steady reduction, then
// the -60 dBFS pilot: when the reduction falls to half and to a tenth of
// where it was when the input dropped. In eco by default, so nothing but
// the cell is between the input and the output: the standard quality's
// decimator rings for a fraction of a millisecond around a step, which a
// -60 dBFS pilot would read as reduction. The cell's times are physical,
// the same at any rate (checked below, standard against eco).
Release release_after(Suite& s, double fs, const Setting& st, double depth, double hold, float quality = 0.0f) {
    const double level = level_for_gr(st, depth, 1000.0);
    const double tail = 8.0;
    const Burst b = burst(fs, 1000.0, {{0.1, -60.0}, {hold, level}, {tail, -60.0}});
    auto p = measuring(s, fs, 1,
                       {{"peak_reduction", st.peak}, {"mode", st.limit}, {"emphasis", st.emphasis}, {"quality", quality}});
    const Chans y = aligned(*p, b.x);
    const std::vector<double> g = gr_trace(b.x[0], y[0], b.amp);
    const std::size_t r0 = b.edges[2];
    Release r{g[r0 - 1], -1.0, -1.0, {}};
    for (std::size_t n = r0; n < g.size(); ++n) {
        const double t = static_cast<double>(n - r0) / fs;
        if (r.t50 < 0.0 && g[n] <= 0.5 * r.gr0) r.t50 = t;
        if (r.t90 < 0.0 && g[n] <= 0.1 * r.gr0) r.t90 = t;
        if ((n - r0) % static_cast<std::size_t>(0.005 * fs) == 0) r.trace.push_back(g[n]);
    }
    return r;
}

void release(Suite& s) {
    const double t50_lo = s.bar("release.t50_min_ms"), t50_hi = s.bar("release.t50_max_ms");
    const double t90_lo = s.bar("release.t90_min_s"), t90_hi = s.bar("release.t90_max_s");
    const double rate_tol = s.bar("release.rate_tolerance");
    const Setting st = kCompress100;
    struct Case {
        double depth, hold;
    };
    const Case cases[] = {{10.0, 1.0}, {10.0, 5.0}, {10.0, 20.0}, {5.0, 5.0}, {20.0, 5.0}};
    json::Value rows = json::Value::array();
    std::vector<Release> at48;
    for (const Case& c : cases) {
        const Release r = release_after(s, 48000.0, st, c.depth, c.hold);
        at48.push_back(r);
        const std::string what = fmt("release after %.0f s at %.0f dB", c.hold, c.depth) +
                                 fmt(" (%.2f dB when the input dropped), eco, 48 kHz", r.gr0);
        s.check(what + ": 50 % release", r.t50 * 1e3, t50_hi, r.t50 * 1e3 >= t50_lo && r.t50 * 1e3 <= t50_hi, "ms",
                fmt("bar %.0f-%.0f ms ([M]: 40-80 ms, 'approximately 0.06 seconds')", t50_lo, t50_hi));
        s.check(what + ": 90 % release", r.t90, t90_hi, r.t90 >= t90_lo && r.t90 <= t90_hi, "s",
                fmt("bar %.1f-%.0f s ([M]: 0.5 to 5 s 'for complete release')", t90_lo, t90_hi));
        json::Value row = json::Value::object();
        row.set("depth_db", jf(c.depth));
        row.set("hold_s", jf(c.hold));
        row.set("gr0_db", jf(r.gr0));
        row.set("t50_ms", jf(r.t50 * 1e3));
        row.set("t90_s", jf(r.t90));
        json::Value tr = json::Value::array();
        for (double v : r.trace) tr.push(jf(v));
        row.set("gr_every_5ms", tr);
        rows.push(row);
    }
    s.data().set("release", rows);
    // The memory: "slower if the unit has either been in compression for a
    // while, or the amount of compression is large" [M].
    const bool longer_hold = at48[0].t90 < at48[1].t90 && at48[1].t90 < at48[2].t90;
    s.check("memory: 90 % release lengthens with the time held (1, 5, 20 s at 10 dB)", at48[2].t90 / at48[0].t90, 1.0,
            longer_hold, "x", fmt("%.2f, %.2f, %.2f s", at48[0].t90, at48[1].t90, at48[2].t90));
    const bool deeper = at48[3].t90 < at48[1].t90 && at48[1].t90 < at48[4].t90;
    s.check("memory: 90 % release lengthens with the depth (5, 10, 20 dB for 5 s)", at48[4].t90 / at48[3].t90, 1.0,
            deeper, "x", fmt("%.2f, %.2f, %.2f s", at48[3].t90, at48[1].t90, at48[4].t90));
    // The cell's times are physical: the same at every rate, and the same
    // in the standard quality, where the cell runs at four times the rate.
    struct Variant {
        double fs;
        float quality;
        const char* name;
    };
    const std::vector<Variant> variants =
        measure::quick() ? std::vector<Variant>{{48000.0, 1.0f, "standard"}}
                         : std::vector<Variant>{{44100.0, 0.0f, "eco"}, {96000.0, 0.0f, "eco"}, {48000.0, 1.0f, "standard"}};
    for (const Variant& v : variants) {
        for (std::size_t i : {std::size_t{0}, std::size_t{2}}) {
            const Release r = release_after(s, v.fs, st, cases[i].depth, cases[i].hold, v.quality);
            const double d = std::fmax(std::fabs(r.t50 / at48[i].t50 - 1.0), std::fabs(r.t90 / at48[i].t90 - 1.0));
            s.check_le(fmt("release after %.0f s at %.0f dB, ", cases[i].hold, cases[i].depth) + v.name + ", " +
                           hz(v.fs) + ": 50 % and 90 % times against eco at 48 kHz",
                       d, rate_tol, "ratio", fmt("%.1f ms, %.3f s", r.t50 * 1e3, r.t90));
        }
    }
}

// ---- attack --------------------------------------------------------------------

void attack(Suite& s) {
    const double max_ms = s.bar("attack.t63_max_ms");
    const Setting st = kCompress100;
    const double fs = 48000.0;
    json::Value rows = json::Value::array();
    double prev = 1e9;
    bool faster = true;
    std::string all;
    for (double depth : {3.0, 6.0, 10.0, 20.0}) {
        const double level = level_for_gr(st, depth, 1000.0);
        const Burst b = burst(fs, 1000.0, {{0.1, -60.0}, {0.5, level}});
        // Eco: as release_after() says.
        auto p = measuring(s, fs, 1, {{"peak_reduction", st.peak}, {"quality", 0.0f}});
        const Chans y = aligned(*p, b.x);
        const std::vector<double> g = gr_trace(b.x[0], y[0], b.amp);
        const std::size_t a0 = b.edges[1];
        const double target = g[a0 + static_cast<std::size_t>(0.3 * fs)];
        double t63 = -1.0, t90 = -1.0;
        json::Value tr = json::Value::array();
        for (std::size_t n = a0; n < g.size(); ++n) {
            const double t = static_cast<double>(n - a0) / fs;
            if (t63 < 0.0 && g[n] >= (1.0 - std::exp(-1.0)) * target) t63 = t;
            if (t90 < 0.0 && g[n] >= 0.9 * target) t90 = t;
            if (n - a0 < static_cast<std::size_t>(0.06 * fs) && (n - a0) % 12 == 0) tr.push(jf(g[n]));
        }
        s.check_le(fmt("attack, a step to %.0f dB of reduction (%.1f dBFS), eco, 48 kHz: 63 %% time", depth, level),
                   t63 * 1e3, max_ms, "ms", fmt("90 %%: %.1f ms; at 0.3 s %.2f dB", t90 * 1e3, target));
        faster &= t63 < prev;
        prev = t63;
        all += fmt("%.2f ", t63 * 1e3);
        json::Value row = json::Value::object();
        row.set("depth_db", jf(depth));
        row.set("t63_ms", jf(t63 * 1e3));
        row.set("t90_ms", jf(t90 * 1e3));
        row.set("gr_every_quarter_ms", tr);
        rows.push(row);
    }
    s.check("attack shortens as the overshoot grows (3, 6, 10, 20 dB)", faster ? 1.0 : 0.0, 1.0, faster, "bool",
            all + "ms");
    s.data().set("attack", rows);
}

// ---- distortion, response, aliasing --------------------------------------------------

void distortion(Suite& s) {
    const double fs = 48000.0;
    const double bar = s.bar("thd.max_pct_1k");
    const double max_gr = s.bar("thd.up_to_gr_db");
    json::Value rows = json::Value::array();
    double worst = 0.0, worst_level = 0.0;
    for (double f : {1000.0, 100.0, 50.0}) {
        for (double level = -40.0; level <= 0.0 + 1e-9; level += measure::quick() ? 10.0 : 4.0) {
            auto p = measuring(s, fs, 1, {{"peak_reduction", 100.0f}});
            const Chans x = measure::sine(fs, f, level, 3.0, 1);
            const Chans y = aligned(*p, x);
            const std::size_t from = static_cast<std::size_t>(2.0 * fs);
            const double thd_pct = 100.0 * std::pow(10.0, measure::thdn_db(y[0], from, fs, f) / 20.0);
            double gr = 0.0;
            steady_out_db(kCompress100, level, f, &gr);
            if (f == 1000.0 && gr <= max_gr && thd_pct > worst) {
                worst = thd_pct;
                worst_level = level;
            }
            json::Value r = json::Value::array();
            r.push(jf(f));
            r.push(jf(level));
            r.push(jf(gr));
            r.push(jf(thd_pct));
            rows.push(r);
        }
    }
    s.check_le(fmt("THD at 1 kHz, compress, peak reduction 100, up to %.0f dB of reduction", max_gr), worst, bar, "%",
               fmt("worst at %.0f dBFS; [M]: under 0.35 %% at +10 dBm out", worst_level));
    s.data().set("thd_f_level_gr_pct", rows);
    for (const auto& r : rows.items()) {
        double f = 0.0, l = 0.0, g = 0.0, t = 0.0;
        r.items()[0].to_double(f);
        r.items()[1].to_double(l);
        r.items()[2].to_double(g);
        r.items()[3].to_double(t);
        if (f < 1000.0 && l == 0.0)
            s.info(fmt("THD at %.0f Hz, 0 dBFS, peak reduction 100 (%.1f dB of reduction)", f, g), t, "%",
                   "the cell follows the low frequencies' ripple");
    }

    // Below threshold the attenuator is a resistive divider with the cell
    // dark: flat. [M]: +-0.1 dB, 30 Hz to 15 kHz.
    double flat = 0.0;
    for (double f : {30.0, 1000.0, 15000.0}) {
        auto p = measuring(s, fs, 1, {{"peak_reduction", 100.0f}});
        const Chans x = measure::sine(fs, f, -60.0, 1.0, 1);
        const Chans y = aligned(*p, x);
        const std::size_t from = static_cast<std::size_t>(0.5 * fs);
        flat = std::fmax(flat, std::fabs(db(measure::rms(y[0], from) / measure::rms(x[0], from))));
    }
    s.check_le("response below threshold (-60 dBFS, peak reduction 100), 30 Hz, 1 kHz, 15 kHz: max |gain|", flat,
               s.bar("response_db"), "dB");
    {
        auto p = measuring(s, fs, 2, {{"peak_reduction", 0.0f}, {"quality", 0.0f}});
        const Chans x = measure::noise(fs, 0.0, 1.0, 11u);
        const Chans y = measure::render(*p, x);
        double d = 0.0;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < x[c].size(); ++n) d = std::fmax(d, std::fabs(y[c][n] - x[c][n]));
        s.check("eco, peak reduction 0 (no compression [M]): output equals input, 0 dBFS noise", d, 0.0, d == 0.0,
                "abs");
    }
}

// Non-harmonic energy in the band on steady tones; the frequencies are
// prime-ish so no alias lands on a harmonic. Nominal: the -18 dBFS
// alignment level (EBU R68) with the peak reduction at 85, about 5 dB of
// reduction there, held to the quality's bar (§3.2: -90 dBc by default,
// -70 dBc in eco). Heavier settings are reported.
void aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 7993.0, 10007.0, 14009.0, 19001.0};
    struct Level {
        const char* name;
        Setting st;
        double dbfs;
    };
    const Level levels[] = {
        {"nominal (-18 dBFS, peak reduction 85, compress)", {"", 0.0f, 85.0f, 0.0f}, -18.0},
        {"-6 dBFS, peak reduction 100, compress", {"", 0.0f, 100.0f, 0.0f}, -6.0},
        {"-6 dBFS, peak reduction 100, limit", {"", 1.0f, 100.0f, 0.0f}, -6.0},
        {"-6 dBFS, peak reduction 100, emphasis 100", {"", 0.0f, 100.0f, 100.0f}, -6.0},
    };
    const std::vector<double> rates =
        measure::quick() ? std::vector<double>{44100.0} : std::vector<double>{44100.0, 48000.0, 96000.0};
    const char* qnames[] = {"eco", "standard (4x)", "high (8x)"};
    const char* bars[] = {"aliasing.eco_dbc", "aliasing.standard_dbc", "aliasing.high_dbc"};
    json::Value rows = json::Value::array();
    for (int q = 0; q < 3; ++q)
    for (double fs : rates) {
        for (std::size_t lv = 0; lv < std::size(levels); ++lv) {
            const Level& L = levels[lv];
            double worst = -400.0, worst_f = 0.0;
            for (double f0 : tones) {
                if (f0 > 0.45 * fs) continue;
                auto p = measuring(s, fs, 1, {{"peak_reduction", L.st.peak}, {"mode", L.st.limit},
                                               {"emphasis", L.st.emphasis}, {"quality", static_cast<float>(q)}});
                const Chans y = aligned(*p, measure::sine(fs, f0, L.dbfs, 1.5, 1));
                const double a = measure::alias_dbc(y[0], static_cast<std::size_t>(0.6 * fs), fs, f0);
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
            double gr = 0.0;
            steady_out_db(L.st, L.dbfs, 1000.0, &gr);
            const std::string what = std::string("aliasing, ") + qnames[q] + ", " + hz(fs) + ", " + L.name +
                                     ", tones 1-19 kHz: worst non-harmonic";
            const std::string note = fmt("at %.0f Hz; %.1f dB of reduction at 1 kHz", worst_f, gr);
            if (lv == 0) s.check_le(what, worst, s.bar(bars[q]), "dBc", note);
            else s.info(what, worst, "dBc", note);
        }
    }
    s.data().set("aliasing", rows);
}

// ---- key, link, hot input --------------------------------------------------------------

void key_and_link(Suite& s) {
    const double fs = 48000.0;
    const Chans quiet = measure::sine(fs, 1000.0, -40.0, 1.0);
    const Chans loud = measure::sine(fs, 1000.0, -6.0, 1.0);
    const std::size_t from = static_cast<std::size_t>(0.8 * fs);
    auto gr_of = [&](const Chans& x, const Chans& y, std::size_t c) {
        return -db(measure::rms(y[c], from) / measure::rms(x[c], from));
    };
    {
        // External key: the side chain hears the key, not the tap.
        auto p = measuring(s, fs, 2, {{"peak_reduction", 100.0f}, {"external_sc", 1.0f}});
        const Chans y = aligned(*p, quiet, &loud);
        const double keyed = gr_of(quiet, y, 0);
        auto q = measuring(s, fs, 2, {{"peak_reduction", 100.0f}});
        const Chans z = aligned(*q, quiet, &loud);
        const double unkeyed = gr_of(quiet, z, 0);
        s.check("external key at -6 dBFS on a -40 dBFS input: reduction", keyed, 10.0, keyed >= 10.0, "dB",
                fmt("key off: %.2f dB", unkeyed));
        s.check_le("external key off, the same input and key: reduction", unkeyed, 0.5, "dB");
    }
    {
        // Linked: the quiet channel follows the loud one.
        Chans x = loud;
        x[1] = quiet[1];
        auto p = measuring(s, fs, 2, {{"peak_reduction", 100.0f}, {"link", 1.0f}});
        const Chans y = aligned(*p, x);
        auto q = measuring(s, fs, 2, {{"peak_reduction", 100.0f}, {"link", 0.0f}});
        const Chans z = aligned(*q, x);
        const double d = std::fabs(gr_of(x, y, 0) - gr_of(x, y, 1));
        s.check_le("stereo link on, -6 and -40 dBFS: |reduction left - right|", d, 1e-6, "dB",
                   fmt("%.2f dB on both", gr_of(x, y, 0)));
        s.check_le("stereo link off: reduction of the -40 dBFS channel", gr_of(x, z, 1), 0.5, "dB",
                   fmt("the -6 dBFS channel %.2f dB", gr_of(x, z, 0)));
    }
}

// Squares at 0, +12 and +24 dBFS, peak reduction 100, the gain at its top
// (+40 dB), each position and full emphasis: the output finite, the
// reduction no deeper than the cell's floor ([M]'s 40 dB), and every
// block inside the 2.67 ms period (minimum of three runs).
void hot_input(Suite& s) {
    const double fs = 48000.0;
    // In phase on both channels: linked, the drivers' grids are joined and
    // see the mean, which anti-phase channels cancel (a physical property
    // of the link, measured by key_and_link()); the last case is
    // anti-phase with the link off.
    const Config configs[] = {{{"mode", 0.0f}, {"quality", 1.0f}}, {{"mode", 1.0f}, {"quality", 1.0f}},
                              {{"emphasis", 100.0f}, {"quality", 1.0f}}, {{"quality", 0.0f}}, {{"quality", 2.0f}},
                              {{"quality", 1.0f}, {"link", 0.0f}}};
    const char* names[] = {"standard, compress", "standard, limit", "standard, emphasis 100", "eco, compress",
                           "high, compress", "standard, link off, channels in anti-phase"};
    for (int ci = 0; ci < 6; ++ci) {
        for (double dbfs : {0.0, 12.0, 24.0}) {
            const double a = std::pow(10.0, dbfs / 20.0);
            Chans x(2, std::vector<float>(static_cast<std::size_t>(1.0 * fs)));
            for (std::size_t n = 0; n < x[0].size(); ++n) {
                const float v = static_cast<float>(
                    std::sin(dsp::kTwoPi * 997.0 * static_cast<double>(n) / fs) >= 0.0 ? a : -a);
                x[0][n] = v;
                x[1][n] = ci == 5 ? -v : v;
            }
            std::vector<double> worst;
            double deepest = 0.0;
            bool finite = true;
            for (int run = 0; run < 3; ++run) {
                auto p = s.make();
                measure::apply(*p, {{"peak_reduction", 100.0f}, {"gain", 40.0f}, {"mix", 100.0f}});
                measure::apply(*p, configs[ci]);
                p->prepare(fs, 128, 2);
                std::vector<double> us;
                measure::RenderOptions opt;
                opt.block_us = &us;
                Chans xp = x;
                for (auto& ch : xp) ch.resize(ch.size() + p->latency(), ch.back());
                Chans y = measure::render(*p, xp, {}, opt);
                for (auto& ch : y) ch.erase(ch.begin(), ch.begin() + p->latency());
                if (run == 0) worst = us;
                for (std::size_t k = 0; k < us.size(); ++k) worst[k] = std::fmin(worst[k], us[k]);
                for (std::size_t c = 0; c < 2; ++c) {
                    for (std::size_t n = 0; n < y[c].size(); ++n) {
                        finite &= std::isfinite(y[c][n]);
                        deepest = std::fmax(deepest, -db(std::fabs(y[c][n] / (x[c][n] * 100.0))));
                    }
                }
            }
            const double w = *std::max_element(worst.begin() + 1, worst.end());
            const std::string what = std::string(names[ci]) + fmt(", %+.0f dBFS square, gain +40 dB", dbfs);
            s.check(what + ": output finite", finite ? 1.0 : 0.0, 1.0, finite, "bool");
            s.check_le(what + ": deepest reduction", deepest, s.bar("deepest_gr_db"), "dB",
                       "the cell under the driver's full swing ([M]: up to 40 dB)");
            s.check_le(what + ": worst block, minimum of 3 runs", w, 2667.0, "us", "the 128-frame period at 48 kHz");
        }
    }
}

// Class A (§3.5) per quality, on the programme signal at the golden
// settings and on a worst case: the programme raised 12 dB at full peak
// reduction, the cell lit on every sample. Which qualities are held to it
// is quality.json's cpu.held (§3.5: a quality over its class ships beside
// an eco mode that meets it); the others are reported.
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
        full.push_back({"peak_reduction", 100.0f});
        measure::check_cpu_on(s, c, programme, names[q], assert_bar);
        measure::check_cpu_on(s, full, hot, std::string(names[q]) + ", programme +12 dB, peak reduction 100",
                              assert_bar);
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture).
measure::Config opto_golden_config() {
    return {{"peak_reduction", 75.0f}, {"gain", 6.0f}, {"emphasis", 30.0f}, {"mix", 90.0f}};
}

int opto(const measure::Args& args) {
    Suite s("opto", [] { return make_opto(); }, args);
    spice(s);
    static_curve(s);
    release(s);
    attack(s);
    distortion(s);
    aliasing(s);
    key_and_link(s);
    hot_input(s);
    const Config golden = opto_golden_config();
    // check_common() without its CPU check, which holds the golden
    // (standard) quality to the class bar: cpu_qualities() instead.
    std::vector<std::pair<std::string, Config>> latency;
    for (int q = 0; q < 3; ++q) {
        const float qf = static_cast<float>(q);
        const std::string n = kQualityIds[q];
        latency.push_back({n + ", compress", {{"quality", qf}}});
        latency.push_back({n + ", limit, emphasis 100", {{"quality", qf}, {"mode", 1.0f}, {"emphasis", 100.0f}}});
        latency.push_back({n + ", external key", {{"quality", qf}, {"external_sc", 1.0f}}});
    }
    measure::check_latency(s, latency);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, {{"peak_reduction", 70.0f}}, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
