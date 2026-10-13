// SPDX-License-Identifier: GPL-3.0-or-later
//
// The chorus against its bars: the delay exact at zero depth, the
// modulation's range and period measured from an impulse train, and the
// common checks.
#include <cmath>
#include <string>

#include "sonora/measure/measure.h"
#include "sonora/native/chorus.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

void centre_delay(Suite& s) {
    for (double fs : measure::kRates) {
        auto p = s.make();
        measure::apply(*p, {{"depth", 0.0f}, {"delay", 10.0f}, {"voices", 1.0f}, {"mix", 100.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::silence(fs, 0.1);
        x[0][100] = 1.0f;
        const Chans y = measure::render(*p, x);
        std::size_t pk = 0;
        for (std::size_t i = 0; i < y[0].size(); ++i) {
            if (std::fabs(y[0][i]) > std::fabs(y[0][pk])) pk = i;
        }
        const long err = static_cast<long>(pk) - 100 - std::lround(0.010 * fs);
        s.check("depth 0: impulse at the set delay (10 ms), " + std::to_string(static_cast<int>(fs)) + " Hz: error",
                static_cast<double>(err), 0.0, err == 0 && std::fabs(y[0][pk] - 1.0f) < 1e-6f, "samples");
    }
}

void modulation(Suite& s) {
    const double fs = 48000.0;
    const double rate = 2.0, depth_ms = 2.0, delay_ms = 10.0;
    auto p = s.make();
    measure::apply(*p, {{"rate", static_cast<float>(rate)}, {"depth", static_cast<float>(depth_ms)},
                        {"delay", static_cast<float>(delay_ms)}, {"voices", 1.0f}, {"mix", 100.0f}});
    p->prepare(fs, 128, 2);
    // An impulse every 20 ms; each comes back at the delay the LFO had.
    const std::size_t every = static_cast<std::size_t>(0.020 * fs);
    Chans x = measure::silence(fs, 2.0);
    for (std::size_t i = 0; i < x[0].size(); i += every) x[0][i] = 1.0f;
    const Chans y = measure::render(*p, x);
    double lo = 1e9, hi = -1e9;
    std::vector<double> d;
    for (std::size_t i = 0; i + every < x[0].size(); i += every) {
        std::size_t pk = i;
        for (std::size_t k = i; k < i + every; ++k) {
            if (std::fabs(y[0][k]) > std::fabs(y[0][pk])) pk = k;
        }
        // Parabolic refinement of the arrival.
        const double a = y[0][pk - 1], b = y[0][pk], c = y[0][pk + 1];
        const double off = 0.5 * (a - c) / (a - 2.0 * b + c);
        const double ms = (pk - i + off) / fs * 1e3;
        d.push_back(ms);
        lo = std::fmin(lo, ms);
        hi = std::fmax(hi, ms);
    }
    const double tol_ms = 2.0 / fs * 1e3;  // two samples
    s.check_le("modulation: shortest delay - (delay - depth)", std::fabs(lo - (delay_ms - depth_ms)), tol_ms, "ms");
    s.check_le("modulation: longest delay - (delay + depth)", std::fabs(hi - (delay_ms + depth_ms)), tol_ms, "ms");
    // Period from the upward crossings of the centre delay.
    std::vector<double> ups;
    for (std::size_t k = 1; k < d.size(); ++k) {
        if (d[k - 1] < delay_ms && d[k] >= delay_ms) {
            const double f = (delay_ms - d[k - 1]) / (d[k] - d[k - 1]);
            ups.push_back((k - 1 + f) * 0.020);
        }
    }
    const double period = ups.size() >= 2 ? (ups.back() - ups.front()) / (ups.size() - 1) : 0.0;
    s.check_le("modulation: period vs 1 / rate, |measured/set - 1|", std::fabs(period * rate - 1.0), 0.02, "ratio");
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config chorus_golden_config() {
    return {{"rate", 1.3f}, {"depth", 3.0f}, {"delay", 12.0f}, {"voices", 3.0f},
            {"spread", 70.0f}, {"feedback", 20.0f}, {"mix", 60.0f}};
}

int chorus(const measure::Args& args) {
    Suite s("chorus", [] { return make_chorus(); }, args);
    centre_delay(s);
    modulation(s);
    const Config golden = chorus_golden_config();
    measure::check_common(s, golden, {{"dry path, mix 0", {{"mix", 0.0f}}}});
    return s.finish();
}

}  // namespace sonora::native::suites
