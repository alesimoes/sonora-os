// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mix bus against its bars (quality.json): the static curve at every
// stepped ratio, manual attack and release from step tests at three rates,
// auto release on a transient and after sustained reduction, the
// side-chain high-pass, THD+N below threshold, the parallel mix and the
// width stage to the bit, width's own zipper on a stereo signal, and the
// common checks.
#include <cmath>
#include <string>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/mixbus.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

Chans dc(double fs, const std::vector<std::pair<double, double>>& segs) {
    std::vector<Chans> parts;
    for (const auto& [secs, db] : segs) {
        Chans p = measure::silence(fs, secs);
        for (auto& c : p) {
            for (float& v : c) v = static_cast<float>(dsp::db_to_gain(db));
        }
        parts.push_back(std::move(p));
    }
    return measure::concat(parts);
}

std::string rate(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

// DC levels (side-chain high-pass off, so the detector sees the level):
// the settled output level against the Giannoulis et al. gain computer,
// once per stepped ratio.
void static_curve(Suite& s) {
    const double bar = s.bar("mixbus.static_db");
    const double fs = 48000.0;
    double worst = 0.0;
    json::Value pts = json::Value::array();
    const struct {
        float thr, ratio_index, knee;
    } settings[] = {{-30.0f, 0.0f, 6.0f}, {-24.0f, 1.0f, 12.0f}, {-20.0f, 2.0f, 0.0f}, {-10.0f, 3.0f, 3.0f}};
    for (const auto& st : settings) {
        const double ratio = mixbus::kRatios[static_cast<int>(st.ratio_index)];
        for (double level = -40.0; level <= 0.0; level += 2.0) {
            auto p = s.make();
            measure::apply(*p, {{"threshold", st.thr}, {"ratio", st.ratio_index}, {"knee", st.knee},
                                {"attack", 1.0f}, {"release", 50.0f}});
            p->prepare(fs, 128, 2);
            const Chans y = measure::render(*p, dc(fs, {{0.5, level}}));
            const double out_db = dsp::gain_to_db(y[0].back());
            const double want = dsp::compressor_curve_db(level, st.thr, ratio, st.knee);
            worst = std::fmax(worst, std::fabs(out_db - want));
            json::Value pt = json::Value::array();
            pt.push(json::Value::number_float(static_cast<float>(ratio)));
            pt.push(json::Value::number_float(static_cast<float>(level)));
            pt.push(json::Value::number_float(static_cast<float>(out_db)));
            pt.push(json::Value::number_float(static_cast<float>(want)));
            pts.push(std::move(pt));
        }
    }
    s.data().set("static_curve_ratio_in_out_formula", std::move(pts));
    s.check_le("static curve vs gain computer, 4 ratios x 21 levels", worst, bar, "dB");
}

// Time from the step to 63 % of the gain change, in dB, as the detector
// defines its attack and release (Giannoulis et al. §3).
double time_to_63(const std::vector<float>& y, const std::vector<float>& x, std::size_t from,
                  std::size_t to, double fs) {
    auto gdb = [&](std::size_t i) { return dsp::gain_to_db(std::fabs(y[i] / x[i])); };
    const double g0 = gdb(from - 1), g1 = gdb(to - 1);
    const double mark = g0 + (1.0 - std::exp(-1.0)) * (g1 - g0);
    for (std::size_t i = from; i < to; ++i) {
        if ((g1 < g0 && gdb(i) <= mark) || (g1 > g0 && gdb(i) >= mark)) return (i - from + 1) / fs;
    }
    return -1.0;
}

const Config kStepCurve = {{"threshold", -20.0f}, {"ratio", 2.0f}, {"knee", 0.0f}};

void attack_release(Suite& s) {
    const double tol = s.bar("mixbus.time_tolerance");
    for (double fs : measure::kRates) {
        for (const auto& [a, r] : {std::pair<float, float>{0.3f, 50.0f}, {10.0f, 300.0f}, {30.0f, 1200.0f}}) {
            auto p = s.make();
            measure::apply(*p, kStepCurve);
            measure::apply(*p, {{"attack", a}, {"release", r}});
            p->prepare(fs, 128, 2);
            const double len = 6.0 * r * 1e-3 + 0.2;
            const Chans x = dc(fs, {{0.2, -40.0}, {len, -10.0}, {len, -40.0}});
            const Chans y = measure::render(*p, x);
            const auto n0 = static_cast<std::size_t>(0.2 * fs);
            const auto n1 = n0 + static_cast<std::size_t>(len * fs);
            const double ta = time_to_63(y[0], x[0], n0, n1, fs);
            const double tr = time_to_63(y[0], x[0], n1, y[0].size(), fs);
            s.check_le("attack " + std::to_string(a) + " ms, |measured/set - 1| at " + rate(fs),
                       std::fabs(ta / (a * 1e-3) - 1.0), tol, "ratio", "measured " + std::to_string(ta * 1e3) + " ms");
            s.check_le("release " + std::to_string(r) + " ms, |measured/set - 1| at " + rate(fs),
                       std::fabs(tr / (r * 1e-3) - 1.0), tol, "ratio", "measured " + std::to_string(tr * 1e3) + " ms");
        }
    }
}

// Auto release: after a 10 ms overload the fast path recovers
// (kAutoFastRelease); after 4 s of reduction the slow path, charged, holds
// the recovery to kAutoSlowRelease. The release knob is set far from both
// so a path that ignored the switch would fail.
void auto_release(Suite& s) {
    const double tol = s.bar("mixbus.time_tolerance");
    const double fs = 48000.0;
    const struct {
        const char* what;
        double over_s, want_s;
    } cases[] = {{"after a 10 ms overload", 0.010, mixbus::kAutoFastRelease},
                 {"after 4 s of reduction", 4.0, mixbus::kAutoSlowRelease}};
    for (const auto& c : cases) {
        auto p = s.make();
        measure::apply(*p, kStepCurve);
        measure::apply(*p, {{"attack", 0.3f}, {"release", 600.0f}, {"auto_release", 1.0f}});
        p->prepare(fs, 128, 2);
        const double tail = 6.0 * mixbus::kAutoSlowRelease;
        const Chans x = dc(fs, {{0.2, -40.0}, {c.over_s, -10.0}, {tail, -40.0}});
        const Chans y = measure::render(*p, x);
        const auto n1 = static_cast<std::size_t>(0.2 * fs) + static_cast<std::size_t>(c.over_s * fs);
        const double tr = time_to_63(y[0], x[0], n1, y[0].size(), fs);
        s.check_le(std::string("auto release ") + c.what + ", |measured/expected - 1|",
                   std::fabs(tr / c.want_s - 1.0), tol, "ratio", "measured " + std::to_string(tr * 1e3) + " ms");
    }
}

// Reduction in dB once settled: input over output level, on a tone, over
// the last 0.5 s of a 1.5 s render.
double settled_reduction_db(Suite& s, const Config& c, double f) {
    const double fs = 48000.0;
    auto p = s.make();
    measure::apply(*p, c);
    p->prepare(fs, 128, 2);
    const Chans x = measure::sine(fs, f, -6.0, 1.5);
    const Chans y = measure::render(*p, x);
    const auto from = static_cast<std::size_t>(1.0 * fs);
    return measure::to_db(measure::rms(x[0], from) / measure::rms(y[0], from));
}

// The side-chain high-pass keeps bass from driving the bus compressor and
// leaves the mid-band's reduction alone.
void sidechain_hpf(Suite& s) {
    const Config base = {{"threshold", -30.0f}, {"ratio", 2.0f}, {"knee", 0.0f}, {"attack", 1.0f},
                         {"release", 300.0f}, {"sc_hpf_freq", 150.0f}};
    Config on = base;
    on.push_back({"sc_hpf_on", 1.0f});
    const double lf_off = settled_reduction_db(s, base, 40.0);
    const double lf_on = settled_reduction_db(s, on, 40.0);
    const double mid_off = settled_reduction_db(s, base, 1000.0);
    const double mid_on = settled_reduction_db(s, on, 1000.0);
    s.check("side-chain high-pass at 150 Hz: 40 Hz reduction drops by", lf_off - lf_on,
            s.bar("sc_hpf.min_drop_db"), lf_off - lf_on >= s.bar("sc_hpf.min_drop_db"), "dB",
            "off " + std::to_string(lf_off) + " dB, on " + std::to_string(lf_on) + " dB");
    s.check_le("side-chain high-pass at 150 Hz: 1 kHz reduction moves by", std::fabs(mid_off - mid_on),
               s.bar("sc_hpf.passband_db"), "dB");
}

void unity_mix_width(Suite& s) {
    const double fs = 48000.0;
    const Config transparent = {{"threshold", 0.0f}, {"knee", 0.0f}};
    {
        auto p = s.make();
        measure::apply(*p, transparent);
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, 1000.0, -1.0, 1.2));
        s.check_le("THD+N at -1 dBFS, 1 kHz, below threshold", measure::thdn_db(y[0], 19200, fs, 1000.0),
                   s.bar("mixbus.thdn_db"), "dB");
    }
    const Chans x = measure::noise(fs, -6.0, 0.5, 3u);
    auto worst_diff = [](const Chans& a, const Chans& b) {
        double w = 0.0;
        for (std::size_t c = 0; c < a.size(); ++c) {
            for (std::size_t i = 0; i < a[c].size(); ++i) w = std::fmax(w, std::fabs(a[c][i] - b[c][i]));
        }
        return w;
    };
    {
        // Parallel mix at 0 % is the dry signal, exactly.
        auto p = s.make();
        measure::apply(*p, {{"mix", 0.0f}, {"threshold", -40.0f}, {"ratio", 3.0f}});
        p->prepare(fs, 128, 2);
        const double w = worst_diff(measure::render(*p, x), x);
        s.check("mix 0 % equals the dry input", w, 0.0, w == 0.0, "abs");
    }
    {
        // Width 100 % with nothing over the threshold: the input, to the bit.
        auto p = s.make();
        measure::apply(*p, transparent);
        p->prepare(fs, 128, 2);
        const double w = worst_diff(measure::render(*p, x), x);
        s.check("width 100 %, below threshold, equals the input", w, 0.0, w == 0.0, "abs");
    }
    {
        // Width w on decorrelated noise: L' = M + w S, R' = M - w S.
        const double bar = s.bar("width.max_error");
        double worst = 0.0;
        for (const float wp : {0.0f, 50.0f, 150.0f, 200.0f}) {
            auto p = s.make();
            measure::apply(*p, transparent);
            measure::apply(*p, {{"width", wp}});
            p->prepare(fs, 128, 2);
            const Chans y = measure::render(*p, x);
            const double w = wp * 0.01;
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                const double mid = 0.5 * (static_cast<double>(x[0][i]) + x[1][i]);
                const double side = 0.5 * (static_cast<double>(x[0][i]) - x[1][i]);
                worst = std::fmax(worst, std::fabs(y[0][i] - (mid + w * side)));
                worst = std::fmax(worst, std::fabs(y[1][i] - (mid - w * side)));
            }
            if (wp == 0.0f) {
                double diff = 0.0;
                for (std::size_t i = 0; i < y[0].size(); ++i) diff = std::fmax(diff, std::fabs(y[0][i] - y[1][i]));
                s.check("width 0 %: left equals right (mono)", diff, 0.0, diff == 0.0, "abs");
            }
        }
        s.check_le("width 0, 50, 150, 200 % against mid + w side, mid - w side", worst, bar, "abs");
    }
    {
        // The common zipper sweep feeds the same tone to both channels, where
        // width does nothing. Here the tone is on the left only, so the side
        // is as loud as the mid and every move of width is heard.
        const double tone_bar = s.bar("zipper_dbfs");
        auto p = s.make();
        const std::uint32_t idx = measure::param_index(*p, "width");
        const ParamInfo& info = p->descriptor().params[idx];
        p->prepare(fs, 128, 2);
        Chans in = measure::zipper_tone(fs, 3.25);
        for (float& v : in[1]) v = 0.0f;
        std::vector<measure::Event> ev;
        const double n0 = to_normalized(info, info.def);
        const auto start = static_cast<std::uint64_t>(0.25 * fs);
        for (std::uint64_t f = start; f < in[0].size(); f += 128) {
            const double t = static_cast<double>(f - start) / fs;
            double n = t < n0 ? n0 - t : t < n0 + 1.0 ? t - n0 : t < 2.0 ? 1.0 - (t - n0 - 1.0) : n0;
            n = std::fmin(1.0, std::fmax(0.0, n));
            ev.push_back({f, idx, static_cast<float>(from_normalized(info, n))});
        }
        const Chans y = measure::render(*p, in, ev);
        double worst = 0.0;
        for (const auto& ch : y) worst = std::fmax(worst, measure::hf_residue_peak(ch, fs, start));
        s.check_le("zipper: full-range sweep of 'width', tone on the left only, HF residue peak",
                   measure::to_db(worst), tone_bar, "dBFS");
    }
    {
        // Aliasing of the fastest, deepest reduction on a 5 kHz tone: the
        // gain modulation's sidebands, reported (no oversampling here).
        auto p = s.make();
        measure::apply(*p, {{"threshold", -40.0f}, {"ratio", 3.0f}, {"knee", 0.0f}, {"attack", 0.1f}, {"release", 50.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, 5000.0, -6.0, 1.2));
        s.info("non-harmonic energy, 5 kHz at -6 dBFS, 10:1, 0.1/50 ms", measure::alias_dbc(y[0], 19200, fs, 5000.0),
               "dBc", "gain modulation only; no nonlinearity in the audio path");
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config mixbus_golden_config() {
    return {{"threshold", -20.0f}, {"ratio", 1.0f}, {"knee", 3.0f}, {"attack", 3.0f}, {"release", 200.0f},
            {"auto_release", 1.0f}, {"sc_hpf_on", 1.0f}, {"makeup", 3.0f}, {"mix", 80.0f}, {"width", 120.0f}};
}

int mixbus(const measure::Args& args) {
    Suite s("mixbus", [] { return make_mixbus(); }, args);
    static_curve(s);
    attack_release(s);
    auto_release(s);
    sidechain_hpf(s);
    unity_mix_width(s);
    const Config golden = mixbus_golden_config();
    const Config zipper = {{"threshold", -18.0f}, {"ratio", 2.0f}};
    measure::check_common(s, golden, {{"default", {}}, {"auto release, high-pass", {{"auto_release", 1.0f}, {"sc_hpf_on", 1.0f}}}},
                          &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
