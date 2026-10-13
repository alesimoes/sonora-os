// SPDX-License-Identifier: GPL-3.0-or-later
//
// The compressor against its bars: static curve, attack and release from
// step tests, THD+N at unity, the parallel mix, and the common checks.
#include <cmath>
#include <string>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/compressor.h"

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

// Static curve with DC levels (side-chain high-pass off, so the detector
// sees the level itself): the output level against the gain computer's
// formula, after the detector has settled.
void static_curve(Suite& s) {
    const double bar = s.bar("compressor.static_db");
    const double fs = 48000.0;
    double worst = 0.0;
    json::Value pts = json::Value::array();
    for (const Config& cfg : {Config{{"threshold", -20.0f}, {"ratio", 4.0f}, {"knee", 0.0f}},
                              Config{{"threshold", -30.0f}, {"ratio", 2.0f}, {"knee", 12.0f}},
                              Config{{"threshold", -10.0f}, {"ratio", 20.0f}, {"knee", 6.0f}}}) {
        for (double level = -60.0; level <= 0.0; level += 2.0) {
            auto p = s.make();
            measure::apply(*p, cfg);
            measure::apply(*p, {{"attack", 1.0f}, {"release", 50.0f}});
            p->prepare(fs, 128, 2);
            const Chans y = measure::render(*p, dc(fs, {{0.5, level}}));
            const double out_db = dsp::gain_to_db(y[0].back());
            const double want = dsp::compressor_curve_db(level, p->param(1), p->param(2), p->param(3));
            worst = std::fmax(worst, std::fabs(out_db - want));
            json::Value pt = json::Value::array();
            pt.push(json::Value::number_float(static_cast<float>(level)));
            pt.push(json::Value::number_float(static_cast<float>(out_db)));
            pt.push(json::Value::number_float(static_cast<float>(want)));
            pts.push(std::move(pt));
        }
    }
    s.data().set("static_curve_in_out_formula", std::move(pts));
    s.check_le("static curve vs gain computer, 3 settings x 31 levels", worst, bar, "dB");
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

void attack_release(Suite& s) {
    const double tol = s.bar("compressor.time_tolerance");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto& [a, r] : {std::pair<float, float>{1.0f, 50.0f}, {10.0f, 150.0f}, {100.0f, 1000.0f}}) {
            auto p = s.make();
            measure::apply(*p, {{"threshold", -20.0f}, {"ratio", 4.0f}, {"knee", 0.0f}, {"attack", a}, {"release", r}});
            p->prepare(fs, 128, 2);
            const double len = 6.0 * r * 1e-3 + 0.2;
            const Chans x = dc(fs, {{0.2, -40.0}, {len, -10.0}, {len, -40.0}});
            const Chans y = measure::render(*p, x);
            const auto n0 = static_cast<std::size_t>(0.2 * fs);
            const auto n1 = n0 + static_cast<std::size_t>(len * fs);
            const double ta = time_to_63(y[0], x[0], n0, n1, fs);
            const double tr = time_to_63(y[0], x[0], n1, y[0].size(), fs);
            const std::string at = " at " + std::to_string(static_cast<int>(fs)) + " Hz";
            s.check_le("attack " + std::to_string(a) + " ms, |measured/set - 1|" + at,
                       std::fabs(ta / (a * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(ta * 1e3) + " ms");
            s.check_le("release " + std::to_string(r) + " ms, |measured/set - 1|" + at,
                       std::fabs(tr / (r * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(tr * 1e3) + " ms");
        }
    }
}

void unity_and_mix(Suite& s) {
    const double fs = 48000.0;
    {
        auto p = s.make();
        measure::apply(*p, {{"ratio", 1.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, 1000.0, -1.0, 1.2));
        s.check_le("THD+N at -1 dBFS, 1 kHz, ratio 1:1", measure::thdn_db(y[0], 19200, fs, 1000.0),
                   s.bar("compressor.thdn_db"), "dB");
    }
    {
        // Parallel mix at 0 % is the dry signal, exactly.
        auto p = s.make();
        measure::apply(*p, {{"mix", 0.0f}, {"threshold", -40.0f}, {"ratio", 10.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::noise(fs, -6.0, 0.5, 3u);
        const Chans y = measure::render(*p, x);
        double worst = 0.0;
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t i = 0; i < x[c].size(); ++i) worst = std::fmax(worst, std::fabs(y[c][i] - x[c][i]));
        }
        s.check("mix 0 % equals the dry input", worst, 0.0, worst == 0.0, "abs");
    }
    {
        // Aliasing of fast, deep gain reduction on a 5 kHz tone: reported.
        auto p = s.make();
        measure::apply(*p, {{"threshold", -30.0f}, {"ratio", 20.0f}, {"knee", 0.0f}, {"attack", 0.05f}, {"release", 5.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, 5000.0, -6.0, 1.2));
        s.info("non-harmonic energy, 5 kHz at -6 dBFS, 20:1, 0.05/5 ms", measure::alias_dbc(y[0], 19200, fs, 5000.0),
               "dBc", "fast gain modulation; no oversampling in N0");
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config compressor_golden_config() {
    return {{"threshold", -24.0f}, {"ratio", 3.0f}, {"knee", 6.0f}, {"attack", 5.0f},
            {"release", 120.0f}, {"makeup", 4.0f}, {"sc_hpf_on", 1.0f}, {"mix", 80.0f}};
}

int compressor(const measure::Args& args) {
    Suite s("compressor", [] { return make_compressor(); }, args);
    static_curve(s);
    attack_release(s);
    unity_and_mix(s);
    const Config golden = compressor_golden_config();
    const Config zipper = {{"threshold", -18.0f}, {"ratio", 4.0f}};
    measure::check_common(s, golden, {{"default", {}}, {"RMS, linked 50 %", {{"detector", 1.0f}, {"link", 50.0f}}}},
                          &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
