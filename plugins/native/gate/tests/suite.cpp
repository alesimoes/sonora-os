// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gate and expander against their bars: attack, hold and release from
// step tests, range, hysteresis, the expander's static curve, and the
// common checks.
#include <cmath>
#include <string>
#include <tuple>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/gate.h"

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

double gain_db(const Chans& y, const Chans& x, std::size_t i) {
    return dsp::gain_to_db(std::fabs(y[0][i] / x[0][i]), -400.0);
}

void gate_timing(Suite& s) {
    const double tol = s.bar("gate.time_tolerance");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto& [a, h, r] : {std::tuple<float, float, float>{1.0f, 50.0f, 100.0f},
                                      {0.2f, 0.0f, 400.0f}, {10.0f, 200.0f, 30.0f}}) {
            auto p = s.make();
            measure::apply(*p, {{"threshold", -40.0f}, {"range", -80.0f}, {"hysteresis", 4.0f},
                                {"attack", a}, {"hold", h}, {"release", r}});
            p->prepare(fs, 128, 2);
            const Chans x = dc(fs, {{0.3, -60.0}, {0.5, -20.0}, {3.0, -60.0}});
            const Chans y = measure::render(*p, x);
            const auto on = static_cast<std::size_t>(0.3 * fs);
            const auto off = static_cast<std::size_t>(0.8 * fs);
            const std::string tag = " (a " + std::to_string(a) + ", h " + std::to_string(h) + ", r " +
                                    std::to_string(r) + ") at " + std::to_string(static_cast<int>(fs)) + " Hz";
            // Closed before: the range.
            s.check_le("closed attenuation - range" + tag, std::fabs(gain_db(y, x, on - 1) + 80.0), 0.01, "dB");
            // Attack: from the step to 63 % of the -80 -> 0 dB change.
            std::size_t i = on;
            while (i < off && gain_db(y, x, i) < -80.0 * std::exp(-1.0)) ++i;
            const double ta = (i - on + 1) / fs;
            s.check_le("attack |measured/set - 1|" + tag, std::fabs(ta / (a * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(ta * 1e3) + " ms");
            // Hold: from the drop to the first movement, less the 10 ms
            // envelope's fall from -20 to the close threshold (-44 dB).
            std::size_t j = off;
            while (j < y[0].size() && gain_db(y, x, j) > -1e-6) ++j;
            const double follower = 0.010 * std::log(dsp::db_to_gain(24.0));
            const double th = (j - off) / fs - follower;
            s.check_le("hold |measured - set|" + tag, std::fabs(th - h * 1e-3), 0.001, "s",
                       "measured " + std::to_string(th * 1e3) + " ms");
            // Release: from that first movement to 63 % of 0 -> -80 dB.
            std::size_t k = j;
            while (k < y[0].size() && gain_db(y, x, k) > -80.0 * (1.0 - std::exp(-1.0))) ++k;
            const double tr = (k - j + 1) / fs;
            s.check_le("release |measured/set - 1|" + tag, std::fabs(tr / (r * 1e-3) - 1.0), tol, "ratio",
                       "measured " + std::to_string(tr * 1e3) + " ms");
        }
    }
}

void hysteresis(Suite& s) {
    const double fs = 48000.0;
    auto p = s.make();
    measure::apply(*p, {{"threshold", -40.0f}, {"hysteresis", 4.0f}, {"hold", 0.0f}, {"release", 20.0f}});
    p->prepare(fs, 128, 2);
    // Open at -20, then -42: inside the hysteresis band, so it stays open;
    // then -46: below threshold - hysteresis, so it closes.
    const Chans x = dc(fs, {{0.3, -20.0}, {1.0, -42.0}, {1.0, -46.0}});
    const Chans y = measure::render(*p, x);
    s.check_le("hysteresis: open at -42 dB (threshold -40, hysteresis 4)",
               std::fabs(gain_db(y, x, static_cast<std::size_t>(1.29 * fs))), 0.01, "dB");
    s.check_le("hysteresis: closed at -46 dB",
               std::fabs(gain_db(y, x, static_cast<std::size_t>(2.29 * fs)) + 80.0), 0.01, "dB");
}

void expander_curve(Suite& s) {
    const double fs = 48000.0;
    double worst = 0.0;
    json::Value pts = json::Value::array();
    for (double level = -80.0; level <= 0.0; level += 2.0) {
        auto p = s.make();
        measure::apply(*p, {{"mode", 1.0f}, {"threshold", -40.0f}, {"ratio", 2.0f}, {"knee", 6.0f},
                            {"range", -60.0f}, {"attack", 1.0f}, {"release", 20.0f}});
        p->prepare(fs, 128, 2);
        const Chans x = dc(fs, {{0.5, level}});
        const Chans y = measure::render(*p, x);
        const double out_db = dsp::gain_to_db(y[0].back(), -400.0);
        double want = dsp::expander_curve_db(level, -40.0, 2.0, 6.0);
        if (level - want > 60.0) want = level - 60.0;
        worst = std::fmax(worst, std::fabs(out_db - want));
        json::Value pt = json::Value::array();
        pt.push(json::Value::number_float(static_cast<float>(level)));
        pt.push(json::Value::number_float(static_cast<float>(out_db)));
        pt.push(json::Value::number_float(static_cast<float>(want)));
        pts.push(std::move(pt));
    }
    s.data().set("expander_curve_in_out_formula", std::move(pts));
    s.check_le("expander static curve vs formula, range-limited, 41 levels", worst, s.bar("gate.static_db"), "dB");
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config gate_golden_config() {
    return {{"threshold", -30.0f}, {"range", -40.0f}, {"attack", 0.5f}, {"hold", 10.0f},
            {"release", 60.0f}, {"key_hpf_on", 1.0f}};
}

int gate(const measure::Args& args) {
    Suite s("gate", [] { return make_gate(); }, args);
    gate_timing(s);
    hysteresis(s);
    expander_curve(s);
    const Config golden = gate_golden_config();
    // Closed on the -6 dBFS tone with 20 dB of range, so the tone is
    // audible, range moves it, and a threshold sweep opens the gate; a
    // 10 ms attack, so the opening itself is not the click. A second pass
    // in expander mode makes ratio and knee audible.
    const Config zipper = {{"threshold", -3.0f}, {"range", -20.0f}, {"attack", 10.0f}};
    measure::check_zipper(s, {{"mode", 1.0f}, {"threshold", -3.0f}, {"range", -40.0f}, {"attack", 10.0f}},
                          " (expander)");
    measure::check_common(s, golden, {{"gate", {{"threshold", -60.0f}}}, {"expander", {{"mode", 1.0f}, {"threshold", -60.0f}}}},
                          &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
