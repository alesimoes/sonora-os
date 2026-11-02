// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape model's invariants that need nothing but common/dsp: the
// Langevin function, the Jiles-Atherton integrator against a finely
// substepped render of its own equations, the loop's physical properties,
// and the matched one-pole pair. The processor against its bars is
// plugins/native/tape/tests/suite.cpp.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/tape.h"
// The integrator's internals, for test_exp_neg.
#include "../src/tape_solve.h"

namespace {

using namespace sonora::dsp;

int test_langevin() {
    for (double x : {-30.0, -5.0, -1.0, -0.26, -0.24, -0.001, 0.0, 0.001, 0.24, 0.26, 1.0, 5.0, 30.0}) {
        // Reference: the closed form in long double where it is stable.
        if (std::fabs(x) >= 0.01) {
            const long double xl = x;
            const long double want = 1.0L / std::tanh(xl) - 1.0L / xl;
            CHECK_LE(std::fabs(langevin(x) - static_cast<double>(want)), 1e-12, "langevin");
        }
        const double h = 1e-5;
        const double num = (langevin(x + h) - langevin(x - h)) / (2 * h);
        CHECK_LE(std::fabs(langevin_slope(x) - num), 1e-6 + 1e-6 * std::fabs(num), "langevin slope");
    }
    // The series meets the closed form at the switch.
    CHECK_LE(std::fabs(langevin(0.2499999) - langevin(0.2500001)), 1e-6, "langevin continuity");
    CHECK_LE(std::fabs(langevin_slope(0.2499999) - langevin_slope(0.2500001)), 1e-6, "langevin slope continuity");
    return 0;
}

// The tabulated Langevin function the integrator uses, against the exact
// one: value and slope over 2e6 points of [-20, 20] (both sides of the
// table's end at 16), exact odd symmetry, and no step at the cell edges.
int test_langevin_table() {
    const LangevinTable t;
    double ev = 0.0, es = 0.0;
    constexpr int kPoints = 2000000;
    for (int i = 0; i <= kPoints; ++i) {
        const double x = -20.0 + 40.0 * i / kPoints;
        double l, dl;
        t.eval(x, l, dl);
        ev = std::fmax(ev, std::fabs(l - langevin(x)));
        ev = std::fmax(ev, std::fabs(t.value(x) - langevin(x)));
        es = std::fmax(es, std::fabs(dl - langevin_slope(x)));
        double ln, dln;
        t.eval(-x, ln, dln);
        CHECK(ln == -l && dln == dl);
    }
    std::printf("Langevin table, [-20, 20]: value error %.3g, slope error %.3g\n", ev, es);
    CHECK_LE(ev, LangevinTable::kValueError, "table value error");
    CHECK_LE(es, LangevinTable::kSlopeError, "table slope error");
    // Continuity: a cell's cubic meets the next one's at the node, and
    // the last cell meets the asymptote at kMax (to coth 16 - 1, 2.5e-14).
    double jump = 0.0;
    for (int i = 1; i <= LangevinTable::kCells; ++i) {
        const double x = static_cast<double>(i) / LangevinTable::kPerUnit;
        jump = std::fmax(jump, std::fabs(t.value(std::nextafter(x, 0.0)) - t.value(x)));
    }
    CHECK_LE(jump, 5e-14, "table continuity at the nodes");
    // Odd, exactly: the integrator's symmetry (test_loop) rests on it.
    CHECK(t.value(0.0) == 0.0);
    return 0;
}

// The integrator's own e^-tau (tape_solve.h) against std::exp on the range
// it is called with.
int test_exp_neg() {
    using S = TapeSolve<tape_detail::V1>;
    double worst = 0.0;
    for (int i = 0; i <= 400000; ++i) {
        const double tau = tape_detail::kTauMax * i / 400000.0;
        const double want = std::exp(-tau);
        worst = std::fmax(worst, std::fabs(S::exp_neg(tape_detail::V1(tau)).x - want) / want);
    }
    std::printf("e^-tau, tau in [0, %g]: relative error %.3g\n", tape_detail::kTauMax, worst);
    CHECK_LE(worst, 5e-10, "exp_neg relative error");
    return 0;
}

// The integrator at the substep caps the processor uses (eco 1, standard
// and high 4) against 512 RK4 substeps of the exact equations, at 2x of
// 48 kHz, on tones and levels of the suite's reference chain. The bars
// are the figures measured when this integrator came in, with headroom: a
// change that makes it less accurate fails here first. A hot field stays
// finite and inside Ms, and a non-finite one leaves the state as it was.
int test_integrator_bounded() {
    const double fs = 96000.0;
    struct Case {
        double f, amp;
        std::uint32_t cap;
        double bar;
    };
    // Amplitudes in units of a: the suite's 8 kHz, -12 dBFS at drive 12 dB
    // through the 15 in/s pre-emphasis is about 5 a; its 15 kHz, -18 dBFS
    // at drive 18 dB about 8 a.
    for (const Case& c : {Case{1000.0, 2.0, 1, 1e-7}, Case{8000.0, 5.0, 1, 2e-4}, Case{8000.0, 5.0, 4, 5e-6},
                          Case{15000.0, 8.0, 4, 1e-4}, Case{1000.0, 40.0, 1, 3e-4}}) {
        TapeHysteresis fast, ref;
        fast.set_max_substeps(c.cap);
        double worst = 0.0;
        const int n = static_cast<int>(0.01 * fs);
        for (int i = 0; i < n; ++i) {
            const double h = c.amp * std::sin(kTwoPi * c.f * i / fs);
            const double m = fast.tick(h);
            const double r = ref.tick_with(h, 512);
            if (i > n / 4) worst = std::fmax(worst, std::fabs(m - r));
        }
        std::printf("integrator, %u substeps at most, %g Hz at 96 kHz, H amplitude %g a: %.3g Ms\n", c.cap, c.f, c.amp,
                    worst);
        CHECK_LE(worst, c.bar, "bounded integrator error");
    }
    TapeHysteresis t;
    t.set_max_substeps(2);
    CHECK(t.max_substeps() == 2);
    for (int i = 0; i < 4000; ++i) {
        const double m = t.tick(1e4 * std::sin(kTwoPi * 19000.0 * i / fs));
        CHECK(std::isfinite(m));
        CHECK(std::fabs(m) <= 1.0);
    }
    const double before = t.tick(0.5);
    CHECK(t.tick(std::nan("")) == before);
    CHECK(t.tick(INFINITY) == before);
    CHECK(std::isfinite(t.tick(0.25)));
    return 0;
}

// The two-channel integrator gives each channel the bits tick() gives it
// alone: on signals that take different substep counts in the two lanes,
// reverse at different samples, cross the table's end, go silent, and
// meet a non-finite field in one lane only.
int test_pair_matches_tick() {
    const double fs = 96000.0;
    for (std::uint32_t cap : {1u, 2u, 4u, 16u}) {
        TapeHysteresis a, b, sa, sb;
        for (TapeHysteresis* t : {&a, &b, &sa, &sb}) t->set_max_substeps(cap);
        std::uint32_t lcg = 12345u;
        std::size_t mismatches = 0;
        constexpr int kBlock = 100;
        for (int blk = 0; blk < 200; ++blk) {
            double xa[kBlock], xb[kBlock], ra[kBlock], rb[kBlock];
            for (int k = 0; k < kBlock; ++k) {
                const int i = blk * kBlock + k;
                lcg = lcg * 1664525u + 1013904223u;
                const double noise = (static_cast<double>(lcg >> 8) / 16777216.0 - 0.5);
                xa[k] = 3.0 * std::sin(kTwoPi * 997.0 * i / fs) + 0.5 * noise;
                xb[k] = (i / 4000 % 2 == 0 ? 60.0 : 0.02) * std::sin(kTwoPi * 7993.0 * i / fs);
                if (i >= 12000 && i < 12500) xb[k] = 0.0;
                if (i == 15000) xa[k] = std::nan("");
                // One channel alone: tick() for half the blocks, run() for
                // the other half.
                ra[k] = blk % 2 == 0 ? sa.tick(xa[k]) : xa[k];
                rb[k] = blk % 2 == 0 ? sb.tick(xb[k]) : xb[k];
            }
            if (blk % 2 != 0) {
                sa.run(ra, kBlock);
                sb.run(rb, kBlock);
            }
            tape_run_pair(a, b, xa, xb, kBlock);
            for (int k = 0; k < kBlock; ++k) mismatches += (xa[k] != ra[k]) + (xb[k] != rb[k]);
        }
        std::printf("pair against tick, %u substeps at most: %zu mismatching samples of 40000\n", cap, mismatches);
        CHECK(mismatches == 0);
    }
    return 0;
}

// The adaptive substeps against 512 per sample of the same RK4, on a
// 1 kHz tone at 4x of 48 kHz, from the small-signal to the saturated.
int test_integrator_converges() {
    const double fs = 192000.0;
    for (double amp : {0.05, 0.5, 2.0, 8.0, 40.0}) {
        TapeHysteresis fast, ref;
        for (TapeHysteresis* t : {&fast, &ref}) {
            t->set_params(JilesAthertonParams{});
            t->reset();
        }
        double worst = 0.0;
        for (int n = 0; n < 4 * 192; ++n) {
            const double h = amp * std::sin(kTwoPi * 1000.0 * n / fs);
            const double m = fast.tick(h);
            const double r = ref.tick_with(h, 512);
            if (n > 192) worst = std::fmax(worst, std::fabs(m - r));
        }
        std::printf("integrator vs 512 substeps, H amplitude %g a: %.3g Ms\n", amp, worst);
        CHECK_LE(worst, 2e-4, "integrator error");
    }
    // A hot step (a 20 kHz tone at amplitude 240) is bounded by the
    // substep cap, and stays finite and inside Ms.
    TapeHysteresis t;
    t.set_params(JilesAthertonParams{});
    for (int n = 0; n < 2000; ++n) {
        const double m = t.tick(240.0 * std::sin(kTwoPi * 20000.0 * n / fs));
        CHECK(std::isfinite(m));
        CHECK(std::fabs(m) <= 1.0);
    }
    return 0;
}

int test_loop() {
    TapeHysteresis t;
    t.set_params(JilesAthertonParams{});
    // Odd symmetry: the same history mirrored gives the mirrored M.
    TapeHysteresis u = t;
    u.reset();
    double worst = 0.0;
    for (int n = 0; n < 3000; ++n) {
        const double h = 3.0 * std::sin(kTwoPi * n / 400.0) * (1.0 + 0.3 * std::sin(kTwoPi * n / 1100.0));
        worst = std::fmax(worst, std::fabs(t.tick(h) + u.tick(-h)));
    }
    CHECK_LE(worst, 1e-12, "odd symmetry");
    // Saturates toward Ms, then the remanence on return to H = 0 is
    // positive (there is a loop), and well under the saturation.
    t.reset();
    double m = 0.0;
    for (int i = 0; i <= 4000; ++i) m = t.tick(40.0 * i / 4000.0);
    CHECK(m > 0.95 && m <= 1.0);
    for (int i = 4000; i >= 0; --i) m = t.tick(40.0 * i / 4000.0);
    CHECK(m > 0.02 && m < 0.5);
    std::printf("remanence after saturation: %.3f Ms\n", m);
    // Near the anhysteretic limit (c -> 1) the loop closes: the same
    // return leaves almost nothing.
    JilesAthertonParams q;
    q.c = 0.999;
    t.set_params(q);
    t.reset();
    for (int i = 0; i <= 4000; ++i) m = t.tick(40.0 * i / 4000.0);
    for (int i = 4000; i >= 0; --i) m = t.tick(40.0 * i / 4000.0);
    CHECK_LE(std::fabs(m), 0.01, "remanence at c -> 1");
    // The initial susceptibility is the slope a tiny signal sees.
    t.set_params(JilesAthertonParams{});
    t.reset();
    double peak = 0.0;
    for (int n = 0; n < 800; ++n) peak = std::fmax(peak, t.tick(1e-6 * std::sin(kTwoPi * n / 400.0)));
    CHECK_LE(std::fabs(peak / 1e-6 / t.initial_susceptibility() - 1.0), 0.01, "initial susceptibility");
    return 0;
}

int test_matched_pair() {
    for (double fs : {88200.0, 96000.0, 192000.0, 384000.0}) {
        for (double fz : {0.0, 3183.0, 9095.0}) {
            const OnePoleZero f = design_matched_one_pole(fz, 24000.0, fs);
            // DC unity, Nyquist and mid-band against the analog.
            CHECK_LE(std::fabs(one_pole_zero_magnitude(f, 1e-3, fs) - 1.0), 1e-9, "DC");
            auto analog = [&](double hz) {
                double m = 1.0 / std::sqrt(1.0 + (hz / 24000.0) * (hz / 24000.0));
                if (fz > 0.0) m *= std::sqrt(1.0 + (hz / fz) * (hz / fz));
                return m;
            };
            CHECK_LE(std::fabs(one_pole_zero_magnitude(f, 0.5 * fs, fs) / analog(0.5 * fs) - 1.0), 1e-9, "Nyquist");
            double worst = 0.0;
            for (double hz = 20.0; hz <= 20000.0; hz *= 1.2)
                worst = std::fmax(worst, std::fabs(20.0 * std::log10(one_pole_zero_magnitude(f, hz, fs) / analog(hz))));
            std::printf("matched one-pole fs %g zero %g: worst %.4f dB 20 Hz-20 kHz\n", fs, fz, worst);
            // Exact at DC and Nyquist, between them the error is the cost of
            // a first-order match at the base rate of a 2x oversampler.
            CHECK_LE(worst, fs < 150000.0 ? 0.7 : 0.2, "matched one-pole in band");
            // The exact inverse: filter then inverse is the identity.
            OnePoleZero g = f;
            OnePoleInverse inv = invert(f);
            double err = 0.0;
            for (int n = 0; n < 5000; ++n) {
                const double x = std::sin(0.37 * n) + 0.5 * std::sin(1.9 * n + 1.0);
                err = std::fmax(err, std::fabs(inv.tick(g.tick(x)) - x));
            }
            CHECK_LE(err, 1e-9, "inverse");
        }
    }
    return 0;
}

int test_head() {
    for (TapeSpeed s : {TapeSpeed::Ips7_5, TapeSpeed::Ips15, TapeSpeed::Ips30}) {
        CHECK(tape_head_loss(20.0, s) > 0.999);
        CHECK(tape_head_loss(20000.0, s) < 1.0);
        const double fc = tape_head_loss_corner_hz(s);
        double worst = 0.0;
        for (double hz = 20.0; hz <= 20000.0; hz *= 1.1) {
            const double lp = 1.0 / std::sqrt(1.0 + (hz / fc) * (hz / fc));
            worst = std::fmax(worst, std::fabs(20.0 * std::log10(lp / tape_head_loss(hz, s))));
        }
        std::printf("head loss speed %d: corner %.0f Hz, worst error %.3f dB\n", static_cast<int>(s), fc, worst);
        CHECK_LE(worst, 0.75, "head loss fit");
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_langevin();
    failed += test_langevin_table();
    failed += test_exp_neg();
    failed += test_integrator_converges();
    failed += test_integrator_bounded();
    failed += test_pair_matches_tick();
    failed += test_loop();
    failed += test_matched_pair();
    failed += test_head();
    if (failed == 0) std::puts("test_dsp_tape: ok");
    return failed;
}
