// SPDX-License-Identifier: GPL-3.0-or-later
//
// The electro-optical cell and its circuit: invariants that need nothing
// but common/dsp. The behaviour against the published figures and against
// ngspice is the opto processor's suite (plugins/native/opto/tests/
// suite.cpp), which reads the committed SPICE reference.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "check.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/opto.h"

namespace {

using sonora::dsp::kTwoPi;
using sonora::dsp::OptoCell;
using sonora::dsp::OptoCellParams;
using sonora::dsp::OptoChannel;
using sonora::dsp::OptoCircuit;

// Under constant light the cell settles where the closed form says, the
// traps in equilibrium with the free carriers: the static curve's
// reference (the suite) relies on it.
int test_steady_state() {
    const OptoCellParams p;
    double worst = 0.0;
    for (double g : {0.01, 0.3, 3.0, 30.0}) {
        OptoCell c;
        c.prepare(48000.0, p);
        for (int i = 0; i < 48000 * 120; ++i) c.tick(g);
        const double want = OptoCell::steady_carriers(g, p);
        worst = std::fmax(worst, std::fabs(c.carriers() / want - 1.0));
        // Trap equilibrium: capture equals emission.
        const double cap1 = p.trap1_capture * c.carriers() * (1.0 - c.trap1());
        const double emi1 = p.trap1_density / p.trap1_tau * c.trap1();
        CHECK(std::fabs(cap1 - emi1) <= 1e-6 * cap1);
    }
    std::printf("  steady state after 120 s: worst |n / n_closed_form - 1| = %.3g\n", worst);
    // The step's fixed point is the continuous one; what
    // is left is the slow trap still settling after 120 s.
    CHECK_LE(worst, 1e-6, "steady carriers, relative");
    return 0;
}

// Silence from rest leaves every state exactly at rest (a host's preroll
// changes nothing), and the dark attenuator is unity.
int test_rest_is_silent() {
    OptoChannel ch;
    ch.prepare(48000.0);
    ch.set_peak_share(1.0);
    for (int i = 0; i < 48000; ++i) CHECK(ch.tick(0.0) == 0.0);
    CHECK(ch.cell().carriers() == 0.0 && ch.cell().trap1() == 0.0 && ch.cell().trap2() == 0.0);
    double g, tap;
    ch.attenuate(g, tap);
    CHECK(std::fabs(g - 1.0) <= 1e-15);
    ch.set_limit(1.0);
    ch.attenuate(g, tap);
    CHECK(std::fabs(g - 1.0) <= 1e-15);
    return 0;
}

// The EL law: zero below its floor (no exp, no subnormal), the published
// form above it, monotone.
int test_light() {
    OptoCell c;
    c.prepare(48000.0);
    CHECK(c.light(0.0) == 0.0);
    CHECK(c.light(0.006) == 0.0);
    CHECK(c.light(std::nan("")) == 0.0);
    CHECK(std::fabs(c.light(100.0) - std::exp(-5.5)) <= 1e-15);
    CHECK(c.light(-100.0) == c.light(100.0));
    double prev = 0.0;
    for (double v = 0.01; v < 400.0; v *= 1.01) {
        const double l = c.light(v);
        CHECK(l >= prev);
        CHECK(l == 0.0 || std::isnormal(l));
        prev = l;
    }
    return 0;
}

// Any light, however absurd, keeps the state finite and the occupancies
// in [0, 1]; a NaN drive is no light.
int test_bounded() {
    OptoCell c;
    c.prepare(44100.0);
    for (double g : {1e3, 1e9, 1e300}) {
        for (int i = 0; i < 4410; ++i) c.tick(g);
        CHECK(std::isfinite(c.carriers()) && c.carriers() > 0.0);
        CHECK(c.trap1() >= 0.0 && c.trap1() <= 1.0 && c.trap2() >= 0.0 && c.trap2() <= 1.0);
    }
    OptoChannel ch;
    ch.prepare(48000.0);
    ch.set_peak_share(1.0);
    ch.drive(std::nan(""));
    CHECK(ch.cell().carriers() == 0.0);
    return 0;
}

// The deepest reduction is [M]'s 40 dB: the cell under the driver's full
// swing (a square at the limit, the light constant) in compress.
int test_deepest_reduction() {
    OptoChannel ch;
    ch.prepare(48000.0);
    const double lim = ch.circuit().drive_limit_v;
    for (int i = 0; i < 48000 * 60; ++i) ch.drive(i & 1 ? lim : -lim);
    const double gr = ch.gain_reduction_db();
    std::printf("  deepest reduction (driver at its %.0f V limit, compress): %.2f dB\n", lim, gr);
    CHECK(gr >= 39.5 && gr <= 40.5);
    return 0;
}

// The discretised side-chain network against its analog prototype, from
// 20 Hz to 20 kHz, every rate, pre-emphasis off, half and full: steady
// sines through sidechain(), amplitude from the last whole periods.
int test_sidechain_response() {
    double worst = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double r37 : {0.0, 0.5e6, 1e6}) {
            for (double f : {20.0, 50.0, 108.0, 160.0, 320.0, 1000.0, 5000.0, 12000.0, 20000.0}) {
                OptoChannel ch;
                ch.prepare(fs);
                ch.set_peak_share(1.0);
                ch.set_r37(r37);
                const double g = ch.sidechain_gain();
                const int n = static_cast<int>(fs * 0.5);
                double acc_s = 0.0, acc_c = 0.0;
                const int from = n / 2;
                // A whole number of periods for the projection.
                const int periods = static_cast<int>((n - from) * f / fs);
                const int count = static_cast<int>(periods * fs / f);
                for (int i = 0; i < from + count; ++i) {
                    const double ph = kTwoPi * f * i / fs;
                    const double y = ch.sidechain(std::sin(ph)) / g;
                    if (i >= from) {
                        acc_s += y * std::sin(ph);
                        acc_c += y * std::cos(ph);
                    }
                }
                const double mag = 2.0 * std::sqrt(acc_s * acc_s + acc_c * acc_c) / count;
                const double want = OptoChannel::sidechain_analog_mag(ch.circuit(), r37, f);
                worst = std::fmax(worst, std::fabs(20.0 * std::log10(mag / want)));
            }
        }
    }
    std::printf("  side chain against its analog prototype, 20 Hz-20 kHz: worst %.4f dB\n", worst);
    CHECK_LE(worst, 0.01, "side-chain response error, dB");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_steady_state();
    failed += test_rest_is_silent();
    failed += test_light();
    failed += test_bounded();
    failed += test_deepest_reduction();
    failed += test_sidechain_response();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp opto: all passed\n");
    return 0;
}
