// SPDX-License-Identifier: GPL-3.0-or-later
//
// The triode stage's own invariants. Its accuracy against ngspice is the
// tube processor's suite (plugins/native/tube/tests/suite.cpp), which reads
// the committed SPICE reference; this file needs nothing but common/dsp.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/triode.h"

namespace {

using sonora::dsp::kTwoPi;
using sonora::dsp::TriodeCurrents;
using sonora::dsp::TriodeDevice;
using sonora::dsp::TriodeLaw;
using sonora::dsp::TriodeStage;

// The tabulated Dempwolf law against the exact one over the plane the
// stage visits and beyond, where the table hands over to the exact law.
int test_table_matches_law() {
    TriodeStage st;
    CHECK(st.prepare(192000.0));
    const TriodeDevice d;
    double worst_i = 0.0, worst_d = 0.0;
    for (double vgk = -8.0; vgk <= 6.0; vgk += 0.0123) {
        for (double vpk = 0.0; vpk <= 400.0; vpk += 1.37) {
            const TriodeCurrents a = st.currents(vgk, vpk);
            const TriodeCurrents b = sonora::dsp::triode_currents(TriodeLaw::Dempwolf, d, vgk, vpk);
            worst_i = std::fmax(worst_i, std::fabs(a.ip - b.ip));
            worst_i = std::fmax(worst_i, std::fabs(a.ig - b.ig));
            worst_d = std::fmax(worst_d, std::fabs(a.dip_dvgk - b.dip_dvgk));
        }
    }
    std::printf("  table: max |I - I(exact)| %.3g A, max |dIp/dVgk - exact| %.3g S\n", worst_i, worst_d);
    CHECK_LE(worst_i, 1e-12, "tabulated current error, A");
    CHECK_LE(worst_d, 1e-8, "tabulated derivative error, S");
    return 0;
}

// The derivatives each law reports are the derivatives of its currents
// (central differences), so Newton converges as it should.
int test_derivatives() {
    const TriodeDevice d;
    for (TriodeLaw law : {TriodeLaw::Dempwolf, TriodeLaw::Koren}) {
        double worst = 0.0;
        for (double vgk : {-3.0, -1.5, -0.4, 0.3, 1.2}) {
            for (double vpk : {20.0, 120.0, 250.0}) {
                const double h = 1e-6;
                const TriodeCurrents c = sonora::dsp::triode_currents(law, d, vgk, vpk);
                const auto at = [&](double g, double p) { return sonora::dsp::triode_currents(law, d, g, p); };
                const double dg = (at(vgk + h, vpk).ip - at(vgk - h, vpk).ip) / (2 * h);
                const double dp = (at(vgk, vpk + h).ip - at(vgk, vpk - h).ip) / (2 * h);
                const double dgg = (at(vgk + h, vpk).ig - at(vgk - h, vpk).ig) / (2 * h);
                const double scale = std::fmax(1e-6, std::fabs(dg));
                worst = std::fmax(worst, std::fabs(c.dip_dvgk - dg) / scale);
                worst = std::fmax(worst, std::fabs(c.dip_dvpk - dp) / std::fmax(1e-8, std::fabs(dp)));
                if (std::fabs(vgk) > 1e-3) worst = std::fmax(worst, std::fabs(c.dig_dvgk - dgg) / std::fmax(1e-6, std::fabs(dgg)));
            }
        }
        CHECK_LE(worst, 1e-5, law == TriodeLaw::Dempwolf ? "Dempwolf derivative, relative" : "Koren derivative, relative");
    }
    return 0;
}

// The operating point against ngspice-44.2's .op of the same circuit
// (plugins/native/tube/tests/spice/dempwolf-*.cir): v(a), v(g), v(k), v(p).
int test_operating_point() {
    TriodeStage st;
    CHECK(st.prepare(192000.0));
    std::printf("  operating point: plate %.4f V, cathode %.5f V, grid %.6f V, %.4f mA\n",
                st.op_voltage(TriodeStage::kP), st.op_voltage(TriodeStage::kK), st.op_voltage(TriodeStage::kG),
                st.op_plate_current() * 1e3);
    CHECK_LE(std::fabs(st.op_voltage(TriodeStage::kP) - 170.449302), 1e-4, "plate vs ngspice, V");
    CHECK_LE(std::fabs(st.op_voltage(TriodeStage::kK) - 1.19338085), 1e-6, "cathode vs ngspice, V");
    CHECK_LE(std::fabs(st.op_voltage(TriodeStage::kG) - -0.0820174308), 1e-8, "grid vs ngspice, V");
    CHECK_LE(std::fabs(st.op_voltage(TriodeStage::kA) - -0.0802518892), 1e-8, "grid leak node vs ngspice, V");
    TriodeStage k;
    CHECK(k.prepare(192000.0, TriodeLaw::Koren));
    CHECK(k.op_voltage(TriodeStage::kP) > 100.0 && k.op_voltage(TriodeStage::kP) < 250.0);
    return 0;
}

// Silence at rest is rest: it leaves every bit of the state where it is,
// so a host's preroll of silence cannot change what follows it, and the
// output at rest is one constant, within 1e-7 V of 0 (the solve's
// tolerance; under -210 dBFS after the processor's normalisation by the
// stage gain).
int test_rest_is_silent() {
    for (TriodeLaw law : {TriodeLaw::Dempwolf, TriodeLaw::Koren}) {
        for (double fs : {88200.0, 192000.0, 384000.0}) {
            TriodeStage st;
            CHECK(st.prepare(fs, law));
            const double first = st.tick(0.0);
            bool constant = true;
            for (int n = 0; n < 96000; ++n) constant = constant && st.tick(0.0) == first;
            CHECK(constant);
            CHECK_LE(std::fabs(first), 1e-7, "output at rest, V");
            // Silence, then a tone, against the tone alone: identical.
            TriodeStage a, b;
            CHECK(a.prepare(fs, law) && b.prepare(fs, law));
            for (int n = 0; n < 1000; ++n) a.tick(0.0);
            for (int n = 0; n < 2000; ++n) {
                const double x = std::sin(kTwoPi * 1000.0 * n / fs);
                CHECK(a.tick(x) == b.tick(x));
            }
        }
    }
    return 0;
}

// Small signals: the stage's gain from the run equals the linearised
// analysis that normalises the processor.
int test_small_signal_gain() {
    const double fs = 384000.0;
    TriodeStage st;
    CHECK(st.prepare(fs));
    double mag = 0.0, ph = 0.0;
    st.small_signal_gain(1000.0, mag, ph);
    std::printf("  small-signal gain at 1 kHz: %.4f (%.2f dB), phase %.2f deg\n", mag, 20 * std::log10(mag), ph);
    CHECK(mag > 30.0 && mag < 100.0);
    CHECK(std::fabs(std::fabs(ph) - 180.0) < 5.0);  // inverting
    const double a = 1e-4;
    const int settle = static_cast<int>(0.4 * fs), len = static_cast<int>(0.1 * fs);
    double re = 0.0, im = 0.0;
    for (int n = 0; n < settle + len; ++n) {
        const double y = st.tick(a * std::sin(kTwoPi * 1000.0 * n / fs));
        if (n >= settle) {
            re += y * std::cos(kTwoPi * 1000.0 * n / fs);
            im += y * std::sin(kTwoPi * 1000.0 * n / fs);
        }
    }
    const double run = 2.0 * std::sqrt(re * re + im * im) / len / a;
    CHECK_LE(std::fabs(20 * std::log10(run / mag)), 0.01, "run vs linearised gain, dB");
    return 0;
}

// Absurd inputs: finite output, the iteration bound, and back to rest.
int test_extremes() {
    TriodeStage st;
    CHECK(st.prepare(192000.0));
    for (double a : {100.0, 1e4, 1e6}) {
        for (int n = 0; n < 19200; ++n) {
            const double y = st.tick(a * std::sin(kTwoPi * 3001.0 * n / 192000.0));
            CHECK(std::isfinite(y));
        }
        CHECK(st.max_iterations_seen() <= TriodeStage::kMaxIterations);
    }
    // Then silence: the coupling capacitors discharge, the output returns
    // to rest (time constants of tens of ms; 2 s is tens of them).
    double last = 0.0;
    for (int n = 0; n < 2 * 192000; ++n) last = st.tick(0.0);
    CHECK_LE(std::fabs(last), 1e-6, "output 2 s after an absurd input, V");
    return 0;
}

// Regression (the RT audit of PR #52, high 1 and 2): a full-scale square at
// full drive and above (63 V at 0 dBFS and +36 dB, then 250 V and 1000 V),
// band-limited by the processor's own oversampler, at every factor. Before
// the fix the solve hit its iteration cap unconverged, the state ran away
// to 5e37 V, and blocks took 6.5 to 7.3 ms against a 2.67 ms period, over
// a hundred times a quiet block. Now every sample converges, nothing
// resets, the load voltage stays within twice the supply, and the worst
// block (two stages, the per-block minimum of three runs) stays within
// six times the median block of a quiet square (0.25 V) on the same
// build. A ratio, so it holds under the sanitizers and on any machine;
// the absolute figures are printed (and measured in Release by the tube's
// suite with --cpu).
int test_hot_square_every_factor() {
    const double fs = 48000.0;
    struct Run {
        double peak = 0.0, worst_us = 0.0, median_us = 0.0;
        std::uint32_t most = 0;
        std::uint64_t resets = 0;
    };
    const auto run = [&](std::uint32_t factor, double amp, Run& out) {
        const std::uint32_t blocks = 375;  // 1 s
        std::vector<double> best(blocks, 1e30);
        for (int pass = 0; pass < 3; ++pass) {
            sonora::dsp::Oversampler os;
            if (!os.prepare(factor, 2, 128, 20000.0 / fs)) return false;
            TriodeStage a, b;
            if (!a.prepare(fs * factor) || !b.prepare(fs * factor)) return false;
            std::vector<float> l(128), r(128), ol(128), orr(128);
            for (std::uint32_t blk = 0; blk < blocks; ++blk) {
                for (std::size_t k = 0; k < 128; ++k) {
                    const double t = (blk * 128.0 + static_cast<double>(k)) / fs;
                    const float v = static_cast<float>(std::sin(kTwoPi * 997.0 * t) >= 0.0 ? amp : -amp);
                    l[k] = v;
                    r[k] = -0.9f * v;
                }
                const auto t0 = std::chrono::steady_clock::now();
                double* x0 = os.up(0, l.data(), 128);
                double* x1 = os.up(1, r.data(), 128);
                for (std::uint32_t i = 0; i < 128 * factor; ++i) {
                    TriodeStage::tick2(a, b, x0[i], x1[i], x0[i], x1[i]);
                    out.peak = std::fmax(out.peak, std::fmax(std::fabs(x0[i]), std::fabs(x1[i])));
                }
                os.down(0, ol.data(), 128);
                os.down(1, orr.data(), 128);
                const double us =
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                best[blk] = std::fmin(best[blk], us);
            }
            out.most = std::max(out.most, std::max(a.max_iterations_seen(), b.max_iterations_seen()));
            out.resets += a.resets() + b.resets();
        }
        best.erase(best.begin());  // the first block warms the caches
        out.worst_us = *std::max_element(best.begin(), best.end());
        std::sort(best.begin(), best.end());
        out.median_us = best[best.size() / 2];
        return true;
    };
    for (std::uint32_t factor : {2u, 4u, 8u}) {
        Run quiet;
        CHECK(run(factor, 0.25, quiet));
        for (double amp : {63.0, 250.0, 1000.0}) {
            Run hot;
            CHECK(run(factor, amp, hot));
            const double ratio = hot.worst_us / quiet.median_us;
            std::printf("  %ux, %6.0f V square: peak %.0f V, %u iterations at most, %llu resets, worst block %.0f us "
                        "(%.1f x a quiet block's median, %.0f us)\n",
                        factor, amp, hot.peak, hot.most, static_cast<unsigned long long>(hot.resets), hot.worst_us,
                        ratio, quiet.median_us);
            CHECK(hot.resets == 0);
            CHECK(hot.most < TriodeStage::kMaxIterations);
            CHECK_LE(hot.peak, 2.0 * 250.0, "load voltage, V");
            CHECK_LE(ratio, 6.0, "worst hot block over a quiet block's median");
        }
    }
    return 0;
}

// Two stages in lockstep give each stage exactly what it gives alone, even
// when the two need different numbers of Newton iterations (one quiet,
// one driven into grid current).
int test_tick2_matches_tick() {
    TriodeStage a, b, ra, rb;
    CHECK(a.prepare(176400.0) && b.prepare(176400.0) && ra.prepare(176400.0) && rb.prepare(176400.0));
    std::uint32_t differ = 0;
    for (int n = 0; n < 176400 / 4; ++n) {
        const double xa = 0.05 * std::sin(kTwoPi * 440.0 * n / 176400.0);
        const double xb = 6.0 * std::sin(kTwoPi * 3001.0 * n / 176400.0);
        double ya, yb;
        TriodeStage::tick2(a, b, xa, xb, ya, yb);
        const double za = ra.tick(xa), zb = rb.tick(xb);
        differ += (ya != za) + (yb != zb);
    }
    std::printf("  tick2 against tick: %u samples differ; iterations %u and %u at most\n", differ,
                a.max_iterations_seen(), b.max_iterations_seen());
    CHECK(differ == 0);
    CHECK(a.max_iterations_seen() != b.max_iterations_seen());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_table_matches_law();
    failed += test_derivatives();
    failed += test_operating_point();
    failed += test_rest_is_silent();
    failed += test_small_signal_gain();
    failed += test_extremes();
    failed += test_tick2_matches_tick();
    failed += test_hot_square_every_factor();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp triode: all passed\n");
    return 0;
}
