// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gain computers, detectors, smoothers, delay lines, the LFO and the
// denormal guard: the small blocks every processor stands on.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/denormals.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/lfo.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/smoothing.h"

namespace {

using namespace sonora::dsp;

int test_compressor_curve() {
    // Hard knee: unity below, 1/R above.
    CHECK(std::fabs(compressor_curve_db(-30.0, -20.0, 4.0, 0.0) - -30.0) < 1e-12);
    CHECK(std::fabs(compressor_curve_db(-8.0, -20.0, 4.0, 0.0) - -17.0) < 1e-12);
    // Soft knee: continuous, with continuous slope from 1 to 1/R.
    const double t = -20.0, r = 4.0, w = 12.0;
    for (double x = -40.0; x < 0.0; x += 0.01) {
        const double y0 = compressor_curve_db(x, t, r, w);
        const double y1 = compressor_curve_db(x + 1e-6, t, r, w);
        const double slope = (y1 - y0) / 1e-6;
        CHECK(slope <= 1.0 + 1e-6 && slope >= 1.0 / r - 1e-6);
    }
    CHECK(std::fabs(compressor_curve_db(t - w / 2, t, r, w) - (t - w / 2)) < 1e-9);
    CHECK(std::fabs(compressor_curve_db(t + w / 2, t, r, w) - (t + w / 2 / r)) < 1e-9);
    return 0;
}

int test_expander_curve() {
    const double t = -40.0, r = 3.0, w = 6.0;
    CHECK(std::fabs(expander_curve_db(-20.0, t, r, w) - -20.0) < 1e-12);
    CHECK(std::fabs(expander_curve_db(-50.0, t, r, w) - (t - 10.0 * r)) < 1e-12);
    for (double x = -80.0; x < 0.0; x += 0.01) {
        const double s = (expander_curve_db(x + 1e-6, t, r, w) - expander_curve_db(x, t, r, w)) / 1e-6;
        CHECK(s >= 1.0 - 1e-6 && s <= r + 1e-6);
    }
    return 0;
}

// The attack and release a processor displays are the 63 % times of this
// detector (ADR-0011 §3.2: within 10 %; here exact to a sample).
int test_branching_detector_times() {
    const double fs = 48000.0;
    BranchingSmoother s;
    s.set_times(0.010, 0.100, fs);
    s.reset(0.0);
    int n = 0;
    while (s.process(10.0) < 10.0 * (1.0 - std::exp(-1.0))) ++n;
    CHECK(std::abs(n + 1 - 480) <= 1);
    s.reset(10.0);
    n = 0;
    while (s.process(0.0) > 10.0 * std::exp(-1.0)) ++n;
    CHECK(std::abs(n + 1 - 4800) <= 1);
    return 0;
}

int test_smoother_reaches_target_exactly() {
    LinearSmoother s;
    s.set_ramp_samples(100);
    s.reset(0.0);
    s.set_target(1.0);
    double last = 0.0;
    for (int i = 0; i < 100; ++i) {
        const double v = s.next();
        CHECK(v > last);
        last = v;
    }
    CHECK(s.current() == 1.0 && !s.moving());
    s.set_target(0.25);
    CHECK(s.advance(60) > 0.25);
    CHECK(s.advance(60) == 0.25);
    return 0;
}

int test_delay_line() {
    DelayLine d;
    d.prepare(1000);
    for (int i = 0; i < 2000; ++i) d.push(static_cast<float>(i));
    // Integer reads are exact: the delay effect is sample-accurate.
    CHECK(d.read(0) == 1999.0f);
    CHECK(d.read(1000) == 999.0f);
    // Lagrange-3 is exact on a cubic, so on a ramp at any fraction.
    CHECK(std::fabs(d.read_lagrange3(10.25) - 1988.75f) < 1e-3f);
    CHECK(std::fabs(d.read_linear(10.5) - 1988.5f) < 1e-3f);
    // On a 1 kHz sine at 48 kHz the cubic's error is far below the linear's.
    DelayLine s;
    s.prepare(64);
    double el = 0.0, ec = 0.0;
    const double fs = 48000.0, f = 1000.0, frac = 7.37;
    for (int i = 0; i < 4800; ++i) {
        s.push(static_cast<float>(std::sin(kTwoPi * f * i / fs)));
        if (i < 100) continue;
        const double want = std::sin(kTwoPi * f * (i - frac) / fs);
        el = std::fmax(el, std::fabs(s.read_linear(frac) - want));
        ec = std::fmax(ec, std::fabs(s.read_lagrange3(frac) - want));
    }
    std::printf("fractional delay on 1 kHz at 48 kHz: linear err %.2e, Lagrange-3 err %.2e\n", el, ec);
    CHECK(ec < 1e-4 && ec < el / 10.0);
    return 0;
}

int test_lfo() {
    Lfo l;
    l.set_rate(1.0, 100.0);
    l.reset(0.0);
    CHECK(std::fabs(l.value(LfoShape::Sine)) < 1e-12);
    CHECK(std::fabs(l.value(LfoShape::Triangle)) < 1e-12);
    for (int i = 0; i < 25; ++i) l.advance();
    CHECK(std::fabs(l.value(LfoShape::Sine) - 1.0) < 1e-9);
    CHECK(std::fabs(l.value(LfoShape::Triangle) - 1.0) < 1e-9);
    CHECK(std::fabs(l.value(LfoShape::Sine, 0.5) + 1.0) < 1e-9);
    return 0;
}

int test_denormal_guard() {
    if (!flush_to_zero_supported()) {
        std::printf("FTZ/DAZ not supported on this architecture: reported, not tested\n");
        return 0;
    }
    CHECK(!flush_to_zero_active());
    {
        DenormalGuard g;
        CHECK(flush_to_zero_active());
        volatile double tiny = 1e-300;
        volatile double r = tiny * 1e-20;  // subnormal result, flushed
        CHECK(r == 0.0);
    }
    CHECK(!flush_to_zero_active());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_compressor_curve();
    failed += test_expander_curve();
    failed += test_branching_detector_times();
    failed += test_smoother_reaches_target_exactly();
    failed += test_delay_line();
    failed += test_lfo();
    failed += test_denormal_guard();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp dynamics, delay, smoothing: all passed\n");
    return 0;
}
