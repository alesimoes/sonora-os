// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reverb against its bars (quality.json): the onset exactly at the
// pre-delay; the reverberation time per octave band, ISO 3382-1 T30,
// against the decay the parameters set, at every rate and size; freeze
// lossless and deaf to its input; stability at the top of the decay
// range; width; linearity; and the common checks.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sonora/dsp/fdn.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/reverb.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

long first_nonzero(const std::vector<float>& y) {
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (y[i] != 0.0f) return static_cast<long>(i);
    }
    return -1;
}

std::vector<double> rates() {
    if (measure::quick()) return {48000.0};
    return {44100.0, 48000.0, 96000.0};
}

// Wet only, no pre-delay, filters open: what the decay is measured on.
Config wet_open() {
    return {{"mix", 100.0f}, {"predelay", 0.0f}, {"low_cut", 20.0f}, {"high_cut", 20000.0f}, {"width", 100.0f}};
}

// The pre-delay is where the wet signal starts, to the sample: the onset
// a tempo grid puts it on (ADR-0011 §4.4). The dry path has no latency.
void onset(Suite& s) {
    const double pds[] = {0.0, 10.0, 37.5, 250.0};
    for (double fs : measure::kRates) {
        for (double pd : pds) {
            for (float size : {0.0f, 100.0f}) {
                for (std::uint32_t chs : {2u, 1u}) {
                    if (chs == 1 && (fs != 48000.0 || size != 100.0f)) continue;
                    auto p = s.make();
                    measure::apply(*p, {{"mix", 100.0f}, {"predelay", static_cast<float>(pd)}, {"size", size}});
                    p->prepare(fs, 128, chs);
                    Chans x = measure::silence(fs, pd * 1e-3 + 0.05, chs);
                    for (auto& c : x) c[100] = 1.0f;
                    const Chans y = measure::render(*p, x);
                    const long want = 100 + std::lround(pd * 1e-3 * fs);
                    bool ok = p->latency() == 0;
                    long got = -1;
                    for (const auto& c : y) {
                        got = first_nonzero(c);
                        ok &= got == want;
                    }
                    char what[160];
                    std::snprintf(what, sizeof what, "first wet sample at the pre-delay, %.1f ms, size %.0f %%, %s, %s",
                                  pd, size, chs == 2 ? "stereo" : "mono", hz(fs).c_str());
                    s.check(what, static_cast<double>(got - want), 0.0, ok, "samples",
                            "expected " + std::to_string(want) + ", latency reported " + std::to_string(p->latency()));
                }
            }
        }
    }
}

const double kBands[] = {63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
// An octave band's bandwidth (fc / sqrt 2) times T60, below which a T30 is
// reported and not held to the bar: a decay curve with so few degrees of
// freedom varies between two noise realisations by more than the bar
// (meter_check measures it; ISO 3382-2:2008 treats this uncertainty).
constexpr double kMinBT = 100.0;

// The meter before the reverb: exponentially decaying white noise of a
// known T60 (the ideal diffuse decay), four realisations averaged as the
// reverb's four responses are, through the same T30 routine. What it
// reads wrong here it would read wrong on the reverb.
void meter_check(Suite& s) {
    const double fs = 48000.0;
    std::uint32_t r = 12345u;
    for (double t60 : {0.4, 1.5, 6.0}) {
        const std::size_t n = static_cast<std::size_t>((1.7 * t60 + 0.3) * fs);
        Chans irs(4, std::vector<float>(n));
        const double k = std::log(1e-3) / (t60 * fs);  // amplitude: -60 dB at t60
        for (auto& ir : irs) {
            for (std::size_t i = 0; i < n; ++i) {
                double g = 0.0;
                for (int j = 0; j < 4; ++j) {
                    r ^= r << 13;
                    r ^= r >> 17;
                    r ^= r << 5;
                    g += static_cast<double>(r) / 4294967296.0 - 0.5;
                }
                ir[i] = static_cast<float>(g * std::exp(k * static_cast<double>(i)));
            }
        }
        for (double fc : kBands) {
            const double bt = fc / std::sqrt(2.0) * t60;
            const double got = measure::t30_seconds(irs, fs, fc);
            char what[160];
            std::snprintf(what, sizeof what, "meter: T30 of decaying noise, T60 %.1f s, %g Hz octave (B T60 %.0f)",
                          t60, fc, bt);
            if (bt >= kMinBT) s.check_le(what, std::fabs(got / t60 - 1.0), s.bar("reverb.t60_tolerance"), "relative");
            else s.info(what, got / t60 - 1.0, "relative", "B T60 under the bar's minimum: reported");
        }
    }
}

// The mean line length at a size, the reference for the design curve.
double mean_length(double size_pct, double fs) {
    std::uint32_t m[dsp::Fdn::kLines];
    dsp::Fdn::lengths_for(size_pct / 100.0, fs, m);
    double acc = 0.0;
    for (std::uint32_t i = 0; i < dsp::Fdn::kLines; ++i) acc += m[i];
    return acc / dsp::Fdn::kLines;
}

// T30 per octave band against the decay the parameters ask for, from the
// four responses (left and right input, left and right output). A band is
// held to the bar where the design curve is flat across it (within 3 %
// from edge to edge) and B T60 is at least kMinBT. In a crossover's
// transition an octave band mixes two decays, and its T30 is reported.
// Returns how many bands were held to the bar.
int decay_case(Suite& s, const std::string& name, double fs, const Config& cfg, json::Value* plot) {
    auto p = s.make();
    measure::apply(*p, wet_open());
    measure::apply(*p, cfg);
    p->prepare(fs, 128, 2);
    dsp::DecaySpec spec;
    spec.t60_s = p->param(measure::param_index(*p, "decay"));
    spec.low_ratio = p->param(measure::param_index(*p, "low_ratio"));
    spec.low_hz = p->param(measure::param_index(*p, "low_freq"));
    spec.high_ratio = p->param(measure::param_index(*p, "high_ratio"));
    spec.high_hz = p->param(measure::param_index(*p, "high_freq"));
    const double m = mean_length(p->param(measure::param_index(*p, "size")), fs);
    const double longest = spec.t60_s * std::fmax(1.0, spec.low_ratio);
    Chans y;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        p->reset();
        Chans x = measure::silence(fs, 1.7 * longest + 0.3);
        x[ch][0] = 1.0f;
        Chans out = measure::render(*p, x);
        y.push_back(std::move(out[0]));
        y.push_back(std::move(out[1]));
    }
    const double tol = s.bar("reverb.t60_tolerance");
    int held = 0;
    for (double fc : kBands) {
        if (fc / std::sqrt(2.0) >= 0.45 * fs) continue;
        const double want = dsp::design_t60(spec, fc, fs, m);
        const double lo = dsp::design_t60(spec, fc / std::sqrt(2.0), fs, m);
        const double hi = dsp::design_t60(spec, std::fmin(fc * std::sqrt(2.0), 0.49 * fs), fs, m);
        const bool flat = std::fabs(lo / want - 1.0) <= 0.03 && std::fabs(hi / want - 1.0) <= 0.03;
        const bool enough = fc / std::sqrt(2.0) * want >= kMinBT;
        const double got = measure::t30_seconds(y, fs, fc);
        const double err = got / want - 1.0;
        char what[220];
        std::snprintf(what, sizeof what, "T30 vs set decay, %s, %s, %g Hz octave (design %.3f s, measured %.3f s)",
                      name.c_str(), hz(fs).c_str(), fc, want, got);
        if (flat && enough) {
            s.check_le(what, std::fabs(err), tol, "relative");
            ++held;
        } else {
            s.info(what, err, "relative", flat ? "B T60 under the bar's minimum: reported"
                                               : "crossover transition in the band: reported");
        }
        if (plot != nullptr) {
            json::Value pt = json::Value::array();
            pt.push(json::Value::number_float(static_cast<float>(fc)));
            pt.push(json::Value::number_float(static_cast<float>(got)));
            pt.push(json::Value::number_float(static_cast<float>(want)));
            plot->push(std::move(pt));
        }
    }
    return held;
}

void decay(Suite& s) {
    struct Case {
        std::string name;
        Config cfg;
        bool plot;
    };
    const std::vector<Case> cases = {
        {"flat, decay 0.4 s, size 50 %", {{"decay", 0.4f}, {"low_ratio", 1.0f}, {"high_ratio", 1.0f}, {"size", 50.0f}}, false},
        {"flat, decay 1.5 s, size 50 %", {{"decay", 1.5f}, {"low_ratio", 1.0f}, {"high_ratio", 1.0f}, {"size", 50.0f}}, true},
        {"flat, decay 6 s, size 50 %", {{"decay", 6.0f}, {"low_ratio", 1.0f}, {"high_ratio", 1.0f}, {"size", 50.0f}}, false},
        {"flat, decay 1.5 s, size 0 %", {{"decay", 1.5f}, {"low_ratio", 1.0f}, {"high_ratio", 1.0f}, {"size", 0.0f}}, false},
        {"flat, decay 1.5 s, size 100 %", {{"decay", 1.5f}, {"low_ratio", 1.0f}, {"high_ratio", 1.0f}, {"size", 100.0f}}, false},
        // Each shelf alone, its crossover far from the bands it is held
        // to: the low decay below, the mid above.
        {"low x2 at 1 kHz, decay 2 s", {{"decay", 2.0f}, {"low_ratio", 2.0f}, {"low_freq", 1000.0f}, {"high_ratio", 1.0f}}, true},
        // The high decay above, the mid below.
        {"high x0.3 at 1 kHz, decay 2 s", {{"decay", 2.0f}, {"low_ratio", 1.0f}, {"high_ratio", 0.3f}, {"high_freq", 1000.0f}}, true},
        {"shaped, decay 2 s, low x2 at 200 Hz, high x0.25 at 3 kHz",
         {{"decay", 2.0f}, {"low_ratio", 2.0f}, {"low_freq", 200.0f}, {"high_ratio", 0.25f}, {"high_freq", 3000.0f}}, true},
        {"defaults", {}, true},
    };
    json::Value curves = json::Value::array();
    for (double fs : rates()) {
        for (const Case& c : cases) {
            json::Value pts = json::Value::array();
            const bool plot = c.plot && fs == 48000.0;
            const int held = decay_case(s, c.name, fs, c.cfg, plot ? &pts : nullptr);
            // A case that asserts nothing would pass whatever the reverb
            // did; every case but the two with transitions everywhere holds
            // at least one band.
            if (held == 0 && c.name.rfind("shaped", 0) != 0 && c.name != "defaults") {
                s.fail("decay case '" + c.name + "' at " + hz(fs) + " held no band to the bar");
            }
            if (plot) {
                json::Value o = json::Value::object();
                o.set("curve", json::Value::string(c.name));
                o.set("points_hz_measured_design", std::move(pts));
                curves.push(std::move(o));
            }
        }
    }
    s.data().set("t60_bands", std::move(curves));
}

// Freeze: lossless, so the tail neither grows nor decays; and deaf, so
// what comes in once it is on changes nothing.
void freeze(Suite& s) {
    const double fs = 48000.0;
    const std::uint32_t at = 48000;  // one second, on a block boundary
    Chans in = measure::noise(fs, -12.0, 11.0, 21u);
    auto p = s.make();
    measure::apply(*p, {{"mix", 100.0f}, {"decay", 2.0f}});
    p->prepare(fs, 128, 2);
    const std::uint32_t fz = measure::param_index(*p, "freeze");
    const Chans y = measure::render(*p, in, {{at, fz, 1.0f}});
    double lo = 1e9, hi = -1e9;
    json::Value pts = json::Value::array();
    for (std::size_t w = 0; w < 9; ++w) {
        const std::size_t a = static_cast<std::size_t>(fs * (1.5 + w)), b = a + static_cast<std::size_t>(fs);
        const double db = measure::to_db(std::sqrt(0.5 * (std::pow(measure::rms(y[0], a, b), 2) +
                                                          std::pow(measure::rms(y[1], a, b), 2))));
        lo = std::fmin(lo, db);
        hi = std::fmax(hi, db);
        pts.push(json::Value::number_float(static_cast<float>(db)));
    }
    s.data().set("freeze_rms_db_per_second", std::move(pts));
    s.check_le("freeze: 1 s RMS over 9 s held, max - min", hi - lo, s.bar("reverb.freeze_drift_db"), "dB",
               "lossless network (Schlecht and Habets)");
    // The input from the end of the 50 ms ramp on is ignored.
    Chans cut = in;
    const std::size_t shut = at + static_cast<std::size_t>(std::lround(0.050 * fs));
    for (auto& c : cut) {
        for (std::size_t i = shut; i < c.size(); ++i) c[i] = 0.0f;
    }
    auto q = s.make();
    measure::apply(*q, {{"mix", 100.0f}, {"decay", 2.0f}});
    q->prepare(fs, 128, 2);
    const Chans z = measure::render(*q, cut, {{at, fz, 1.0f}});
    double diff = 0.0;
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t i = shut; i < y[c].size(); ++i) diff = std::fmax(diff, std::fabs(y[c][i] - z[c][i]));
    }
    s.check_le("freeze: input after the ramp changes the output by", diff, s.bar("golden_abs"), "abs",
               "the network takes no input when frozen");
}

// --cpu only. The common CPU check renders with the parameters still; the
// reverb's costliest block is one where the decay moves, and every line's
// absorbent filter is redesigned at each 32-sample control tick (fdn.h).
// The decay and both crossovers are automated on every block here, which
// no host does for long, and the class bar still applies.
void cpu_while_automated(Suite& s) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const double secs = s.args().cpu_seconds;
    const Chans in = measure::noise(fs, -12.0, secs, 31u);
    std::vector<double> best;
    for (int run = 0; run < 5; ++run) {
        auto p = s.make();
        p->prepare(fs, 128, 2);
        const std::uint32_t d = measure::param_index(*p, "decay");
        const std::uint32_t lf = measure::param_index(*p, "low_freq");
        const std::uint32_t hf = measure::param_index(*p, "high_freq");
        std::vector<measure::Event> ev;
        for (std::uint64_t f = 0, k = 0; f < in[0].size(); f += 128, ++k) {
            const float t = static_cast<float>(k % 400) / 400.0f;  // a ramp every 1.07 s
            ev.push_back({f, d, 0.3f + 8.0f * t});
            ev.push_back({f, lf, 80.0f + 600.0f * t});
            ev.push_back({f, hf, 2000.0f + 9000.0f * t});
        }
        std::vector<double> us;
        measure::RenderOptions opt;
        opt.block_us = &us;
        measure::render(*p, in, ev, opt);
        us.erase(us.begin(), us.begin() + static_cast<long>(fs / 128.0));
        if (run == 0) best = us;
        for (std::size_t k = 0; k < us.size() && k < best.size(); ++k) best[k] = std::fmin(best[k], us[k]);
    }
    std::sort(best.begin(), best.end());
    const double p999 = best[static_cast<std::size_t>(std::ceil(0.999 * (best.size() - 1)))];
    const double mx = best.back();
    s.info("CPU per block, decay and crossovers automated every block: p50", best[best.size() / 2], "us",
           "per-block minimum of 5 runs");
    s.check_le("CPU per block, decay and crossovers automated every block: p99.9", p999, s.bar("cpu.p999_us"), "us",
               "per-block minimum of 5 runs");
    s.check_le("CPU per block, decay and crossovers automated every block: max", mx, s.bar("cpu.max_us"), "us",
               "per-block minimum of 5 runs");
}

void stability_width_linearity(Suite& s) {
    const double fs = 48000.0;
    {
        // The longest decay the parameters allow (20 s, 40 s below the low
        // crossover) on 10 s of noise: bounded, since every line's
        // absorption stays under unity (tests/test_fdn.cpp).
        auto p = s.make();
        measure::apply(*p, {{"mix", 100.0f}, {"decay", 20.0f}, {"low_ratio", 2.0f}, {"high_ratio", 1.0f},
                            {"size", 100.0f}, {"diffusion", 100.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::noise(fs, -6.0, 10.0, 4u));
        s.check_le("decay 20 s, low x2, on 10 s of noise: peak output", measure::peak(y[0]), 20.0, "linear",
                   "bounded");
    }
    {
        // Width 0: mono wet. A mono input, so the dry paths are identical
        // and the two outputs must be equal to the bit.
        auto p = s.make();
        measure::apply(*p, {{"mix", 100.0f}, {"width", 0.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::noise(fs, -12.0, 0.5, 8u, 1);
        x = measure::concat({x, measure::silence(fs, 1.0, 1)});
        x.push_back(x[0]);
        const Chans y = measure::render(*p, x);
        double d = 0.0;
        for (std::size_t i = 0; i < y[0].size(); ++i) d = std::fmax(d, std::fabs(y[0][i] - y[1][i]));
        s.check("width 0 %: left equals right", d, 0.0, d == 0.0, "abs");
    }
    {
        // Width 100 %: on a mono impulse the two tails are decorrelated
        // (orthogonal output vectors, fdn.h). Normalised cross-correlation
        // at lag 0 over the tail, 50 ms to 1 s.
        auto p = s.make();
        measure::apply(*p, {{"mix", 100.0f}, {"width", 100.0f}, {"predelay", 0.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::silence(fs, 1.2);
        x[0][0] = x[1][0] = 1.0f;
        const Chans y = measure::render(*p, x);
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.05 * fs); i < static_cast<std::size_t>(fs); ++i) {
            lr += static_cast<double>(y[0][i]) * y[1][i];
            ll += static_cast<double>(y[0][i]) * y[0][i];
            rr += static_cast<double>(y[1][i]) * y[1][i];
        }
        s.check_le("width 100 %: |interchannel correlation| of the tail, mono impulse", std::fabs(lr / std::sqrt(ll * rr)),
                   s.bar("reverb.tail_correlation"), "");
    }
    {
        // Linear: the wet steady state of a sine is a sine. Decay 1 s, so
        // the onset transient is 240 dB down when the window starts.
        auto p = s.make();
        measure::apply(*p, {{"mix", 100.0f}, {"decay", 1.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, 1000.0, -1.0, 5.0));
        s.check_le("THD+N, 1 kHz at -1 dBFS in, wet steady state", measure::thdn_db(y[0], 4 * 48000, fs, 1000.0),
                   s.bar("reverb.thdn_db"), "dB");
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config reverb_golden_config() {
    return {{"predelay", 12.0f}, {"decay", 1.2f}, {"size", 55.0f}, {"diffusion", 80.0f},
            {"low_ratio", 1.3f}, {"low_freq", 300.0f}, {"high_ratio", 0.45f}, {"high_freq", 4500.0f},
            {"low_cut", 80.0f}, {"high_cut", 12000.0f}, {"width", 90.0f}, {"mix", 35.0f}};
}

int reverb(const measure::Args& args) {
    Suite s("reverb", [] { return make_reverb(); }, args);
    onset(s);
    meter_check(s);
    decay(s);
    freeze(s);
    stability_width_linearity(s);
    cpu_while_automated(s);
    const Config golden = reverb_golden_config();
    const Config zipper = {{"mix", 50.0f}, {"decay", 1.5f}};
    measure::check_common(s, golden,
                          {{"dry path, mix 0", {{"mix", 0.0f}}},
                           {"dry path, mix 0, size 100 %, pre-delay 250 ms",
                            {{"mix", 0.0f}, {"size", 100.0f}, {"predelay", 250.0f}}}},
                          &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
