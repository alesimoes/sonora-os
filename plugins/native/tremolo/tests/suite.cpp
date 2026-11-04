// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tremolo and auto-pan against their bars. A DC input of 1.0 makes the
// output the gain itself, so the modulation is measured directly: its range
// for every shape, its period, the channels' gains at full spread, the
// auto-pan's constant power and extremes, and depth 0 leaving the signal
// bit-exact. Then the common checks (latency, non-finite, reset, denormals,
// zipper, golden).
#include <cmath>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/tremolo.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

const char* const kShapeNames[] = {"sine", "triangle", "soft square"};

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

Chans dc(double fs, double seconds) {
    Chans x = measure::silence(fs, seconds);
    for (auto& ch : x) {
        for (float& v : ch) v = 1.0f;
    }
    return x;
}

// Rate 5 Hz puts the LFO's peaks on whole samples at every rate (the
// periods are 8820, 9600 and 19200 samples), so the range is measured
// exactly and not a fraction of a sample from its top.
void range(Suite& s) {
    for (double fs : measure::kRates) {
        for (int shape = 0; shape < 3; ++shape) {
            for (float depth : {30.0f, 100.0f}) {
                auto p = s.make();
                measure::apply(*p, {{"mode", 0.0f}, {"rate", 5.0f}, {"depth", depth},
                                    {"shape", static_cast<float>(shape)}, {"spread", 0.0f}});
                p->prepare(fs, 128, 2);
                const Chans y = measure::render(*p, dc(fs, 1.0));
                double lo = 1e9, hi = -1e9;
                for (float v : y[0]) {
                    lo = std::fmin(lo, v);
                    hi = std::fmax(hi, v);
                }
                const double d = depth * 0.01;
                const double err = std::fmax(std::fabs(hi - 1.0), std::fabs(lo - (1.0 - d)));
                s.check_le(std::string("tremolo: gain range [1 - depth, 1], ") + kShapeNames[shape] + ", depth " +
                               std::to_string(static_cast<int>(depth)) + " %, " + hz(fs),
                           err, s.bar("tremolo.range_err"), "linear");
            }
        }
    }
}

// Upward crossings of the gain's mid level, interpolated, against 1 / rate.
void period(Suite& s) {
    const double rate = 3.3;
    for (double fs : measure::kRates) {
        for (int shape = 0; shape < 3; ++shape) {
            auto p = s.make();
            measure::apply(*p, {{"mode", 0.0f}, {"rate", static_cast<float>(rate)}, {"depth", 60.0f},
                                {"shape", static_cast<float>(shape)}});
            p->prepare(fs, 128, 2);
            const Chans y = measure::render(*p, dc(fs, 5.0));
            const double mid = 1.0 - 0.3;
            std::vector<double> ups;
            for (std::size_t i = 1; i < y[0].size(); ++i) {
                if (y[0][i - 1] < mid && y[0][i] >= mid) {
                    ups.push_back(static_cast<double>(i - 1) + (mid - y[0][i - 1]) / (y[0][i] - y[0][i - 1]));
                }
            }
            double err = 1e9;
            if (ups.size() >= 3) {
                const double per = (ups.back() - ups.front()) / static_cast<double>(ups.size() - 1) / fs;
                err = std::fabs(per * rate - 1.0);
            }
            s.check_le(std::string("tremolo: period vs 1 / rate, ") + kShapeNames[shape] + ", " + hz(fs), err,
                       s.bar("tremolo.period_err"), "ratio");
        }
    }
}

// At 100 % spread the channels are half a cycle apart; every shape is odd
// about its quarter point, so the two gains sum to 2 - depth.
void spread(Suite& s) {
    const double fs = 48000.0;
    for (int shape = 0; shape < 3; ++shape) {
        auto p = s.make();
        measure::apply(*p, {{"mode", 0.0f}, {"rate", 3.3f}, {"depth", 70.0f},
                            {"shape", static_cast<float>(shape)}, {"spread", 100.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, dc(fs, 2.0));
        double err = 0.0;
        for (std::size_t i = 0; i < y[0].size(); ++i) {
            err = std::fmax(err, std::fabs(static_cast<double>(y[0][i]) + y[1][i] - 1.3));
        }
        s.check_le(std::string("tremolo: gL + gR at 100 % spread vs 2 - depth, ") + kShapeNames[shape], err,
                   s.bar("tremolo.spread_err"), "linear");
    }
}

// The pan law: gL^2 + gR^2 = 2 whatever the position, and at depth 100 %
// the extremes are hard left (sqrt2, 0) and hard right (0, sqrt2).
void auto_pan(Suite& s) {
    const double fs = 48000.0, root2 = std::sqrt(2.0);
    for (int shape = 0; shape < 3; ++shape) {
        auto p = s.make();
        measure::apply(*p, {{"mode", 1.0f}, {"rate", 5.0f}, {"depth", 100.0f}, {"shape", static_cast<float>(shape)}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, dc(fs, 1.0));
        double power = 0.0, lo_l = 1e9, hi_l = -1e9, lo_r = 1e9, hi_r = -1e9;
        for (std::size_t i = 0; i < y[0].size(); ++i) {
            const double l = y[0][i], r = y[1][i];
            power = std::fmax(power, std::fabs(l * l + r * r - 2.0));
            lo_l = std::fmin(lo_l, l);
            hi_l = std::fmax(hi_l, l);
            lo_r = std::fmin(lo_r, r);
            hi_r = std::fmax(hi_r, r);
        }
        s.check_le(std::string("auto-pan: |gL^2 + gR^2 - 2|, ") + kShapeNames[shape], power,
                   s.bar("tremolo.pan_power_err"), "linear");
        const double ext = std::fmax(std::fmax(std::fabs(lo_l), std::fabs(lo_r)),
                                     std::fmax(std::fabs(hi_l - root2), std::fabs(hi_r - root2)));
        s.check_le(std::string("auto-pan: extremes 0 and sqrt2 at depth 100 %, ") + kShapeNames[shape], ext,
                   s.bar("tremolo.pan_range_err"), "linear");
    }
    // Depth 0 is the centre: unity on both channels.
    auto p = s.make();
    measure::apply(*p, {{"mode", 1.0f}, {"depth", 0.0f}});
    p->prepare(fs, 128, 2);
    const Chans y = measure::render(*p, dc(fs, 0.5));
    double err = 0.0;
    for (std::size_t i = 0; i < y[0].size(); ++i) {
        err = std::fmax(err, std::fmax(std::fabs(y[0][i] - 1.0), std::fabs(y[1][i] - 1.0)));
    }
    s.check_le("auto-pan: depth 0 is unity on both channels", err, s.bar("tremolo.pan_range_err"), "linear");
}

void unity(Suite& s) {
    const double fs = 48000.0;
    auto p = s.make();
    measure::apply(*p, {{"mode", 0.0f}, {"depth", 0.0f}, {"rate", 7.0f}});
    p->prepare(fs, 128, 2);
    const Chans x = measure::noise(fs, -3.0, 1.0, 11);
    const Chans y = measure::render(*p, x);
    double err = 0.0;
    for (std::uint32_t c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < x[c].size(); ++i) err = std::fmax(err, std::fabs(y[c][i] - x[c][i]));
    }
    s.check_le("tremolo: depth 0 leaves the signal bit-exact", err, s.bar("tremolo.unity_err"), "linear");
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config tremolo_golden_config() {
    return {{"mode", 0.0f}, {"rate", 3.3f}, {"depth", 70.0f}, {"shape", 1.0f}, {"spread", 40.0f}};
}

int tremolo(const measure::Args& args) {
    Suite s("tremolo", [] { return make_tremolo(); }, args);
    range(s);
    period(s);
    spread(s);
    auto_pan(s);
    unity(s);
    const Config golden = tremolo_golden_config();
    const Config pan_cfg = {{"mode", 1.0f}, {"rate", 3.3f}, {"depth", 80.0f}, {"shape", 2.0f}};
    measure::check_common(s, golden, {{"dry path, depth 0", {{"depth", 0.0f}}}, {"auto-pan", pan_cfg}}, &golden);
    measure::check_zipper(s, pan_cfg, " (auto-pan)");
    return s.finish();
}

}  // namespace sonora::native::suites
