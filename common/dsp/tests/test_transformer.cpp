// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage (transformer.h): the core's physics and the supply's
// sag, measured with nothing but common/dsp. The processor-level bars
// (aliasing, latency, golden) are the tube suite's.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/transformer.h"

namespace {

using sonora::dsp::kTwoPi;
using sonora::dsp::OutputStage;

constexpr double kFs = 192000.0;

struct Run {
    std::vector<double> y;
    double rail_min = 1e9, rail_end = 0.0;
    double psi_peak = 0.0, psi_min = 0.0, psi_max = 0.0;
    double area = 0.0;  // the last cycle's integral of i d(psi), joules per ohm-ish: sign is what matters
    std::uint32_t iters = 0;
};

// `cycles` cycles of a sine at `f`, after `settle` cycles that are not
// recorded; the stage is used as left (no reset), so a caller can chain.
Run drive(OutputStage& st, double f, double amp, int settle, int cycles) {
    Run r;
    const int per = static_cast<int>(std::lround(kFs / f));
    double in[2], out[2];
    double last_psi = st.flux(0);
    for (int n = 0; n < (settle + cycles) * per; ++n) {
        in[0] = in[1] = amp * std::sin(kTwoPi * n / per);
        st.tick(in, out);
        if (n >= settle * per) {
            r.y.push_back(out[0]);
            r.rail_min = std::fmin(r.rail_min, st.rail_voltage());
            r.psi_min = std::fmin(r.psi_min, st.flux(0));
            r.psi_max = std::fmax(r.psi_max, st.flux(0));
            r.psi_peak = 0.5 * (r.psi_max - r.psi_min);
        }
        if (n >= (settle + cycles - 1) * per) r.area += st.magnetising_current(0) * (st.flux(0) - last_psi);
        last_psi = st.flux(0);
    }
    r.rail_end = st.rail_voltage();
    r.iters = st.max_iterations_seen();
    return r;
}

std::complex<double> harmonic(const std::vector<double>& y, int per, int k) {
    std::complex<double> acc = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n)
        acc += y[n] * std::exp(std::complex<double>(0.0, -kTwoPi * k * static_cast<double>(n % per) / per));
    return 2.0 * acc / static_cast<double>(y.size());
}

// Total harmonic distortion, harmonics 2..9, as a ratio.
double thd(const std::vector<double>& y, int per, double* h1 = nullptr) {
    const double a1 = std::abs(harmonic(y, per, 1));
    double s = 0.0;
    for (int k = 2; k <= 9; ++k) s += std::norm(harmonic(y, per, k));
    if (h1) *h1 = a1;
    return std::sqrt(s) / a1;
}

OutputStage make(double sag) {
    OutputStage st;
    st.prepare(kFs, 2, {}, {});
    st.set_sag(sag);
    return st;
}

int test_rest() {
    OutputStage st = make(1.0);
    double in[2] = {0, 0}, out[2];
    for (int n = 0; n < 4800; ++n) {
        st.tick(in, out);
        CHECK(out[0] == 0.0 && out[1] == 0.0);
    }
    CHECK(std::fabs(st.rail_voltage() - (st.idle_rail_voltage() - 40.0)) < 1e-9);
    return 0;
}

// The response at a low level, where the core is in its Rayleigh region
// (reversible, plus the first irreversible steps): reported; the corner
// moves up with the level as the core's differential inductance falls.
int test_low_level_response() {
    for (double amp : {0.001, 0.05}) {
        for (double f : {20.0, 40.0, 80.0, 160.0, 1000.0}) {
            OutputStage st = make(0.0);
            const Run r = drive(st, f, amp, 10, 4);
            double h1 = 0.0;
            const int per = static_cast<int>(std::lround(kFs / f));
            thd(r.y, per, &h1);
            std::printf("  %7.0f Hz at %g of full scale: gain %.2f dB\n", f, amp, 20.0 * std::log10(h1 / amp));
        }
    }
    return 0;
}

// Mid-band: unity (the mid-band transfer is divided out) and nearly linear;
// the core's hysteresis and curvature are a bass phenomenon.
int test_mid_band_and_lf_first() {
    double thd_at[4];
    const double freqs[4] = {1000.0, 200.0, 80.0, 40.0};
    for (int i = 0; i < 4; ++i) {
        OutputStage st = make(0.0);
        const int per = static_cast<int>(std::lround(kFs / freqs[i]));
        const Run r = drive(st, freqs[i], 0.5, 6, 8);
        double h1 = 0.0;
        thd_at[i] = thd(r.y, per, &h1);
        std::printf("  %6.0f Hz, 0.5 of full scale: gain %.3f dB, THD %.3f %%, peak flux %.4f V*s, solve <= %u iterations\n",
                    freqs[i], 20.0 * std::log10(h1 / 0.5), 100.0 * thd_at[i], r.psi_peak, r.iters);
        CHECK(r.iters <= OutputStage::kMaxIter);
        if (i == 0) CHECK(std::fabs(20.0 * std::log10(h1 / 0.5)) < 0.5);
    }
    // THD rises monotonically as the frequency falls: LF first.
    CHECK(thd_at[0] < thd_at[1] && thd_at[1] < thd_at[2] && thd_at[2] < thd_at[3]);
    CHECK_LE(thd_at[0], 0.02, "THD at 1 kHz, 0.5 FS");
    CHECK(thd_at[3] > 3.0 * thd_at[0]);
    return 0;
}

// The flux is the integral of the voltage: at a fixed level, halving the
// frequency doubles it (while the core is not saturated).
int test_flux_follows_one_over_f() {
    OutputStage a = make(0.0), b = make(0.0);
    const Run ra = drive(a, 400.0, 0.2, 6, 4);
    const Run rb = drive(b, 200.0, 0.2, 6, 4);
    const double ratio = rb.psi_peak / ra.psi_peak;
    std::printf("  peak flux ratio, 200 Hz over 400 Hz: %.3f (1/f: 2)\n", ratio);
    CHECK(std::fabs(ratio - 2.0) < 0.1);
    return 0;
}

// Hysteresis dissipates: the loop of i against psi is traversed
// anticlockwise, the integral of i d(psi) over a periodic cycle is
// positive (the iron's loss; it is the reactive energy plus this). With
// a stiffer coercivity it grows.
int test_hysteresis_dissipates() {
    OutputStage st = make(0.0);
    const Run r = drive(st, 40.0, 0.5, 6, 3);
    OutputStage st2;
    sonora::dsp::CoreParams soft;
    soft.k = 0.0006;
    st2.prepare(kFs, 2, {}, soft);
    st2.set_sag(0.0);
    const Run r2 = drive(st2, 40.0, 0.5, 6, 3);
    std::printf("  loop integral of i d(psi), 40 Hz: k=6 mA %.4g, k=0.6 mA %.4g (V*s*A)\n", r.area, r2.area);
    CHECK(r.area > 0.0);
    CHECK(r.area > 3.0 * r2.area);
    return 0;
}

// Sag: a loud burst pulls the reservoir down, the rail with it; a stiff
// supply does not move; the reservoir recovers after the burst.
int test_sag() {
    OutputStage stiff = make(0.0);
    const Run a = drive(stiff, 100.0, 1.0, 4, 10);
    CHECK(std::fabs(a.rail_min - (stiff.idle_rail_voltage() - 40.0)) < 1e-9);

    OutputStage st = make(1.0);
    const Run b = drive(st, 100.0, 1.0, 4, 10);
    const double idle = st.idle_rail_voltage() - 40.0;
    std::printf("  rail at idle %.1f V; under a full-scale 100 Hz burst, sag 100 %%: min %.1f V (%.1f %% down)\n", idle,
                b.rail_min, 100.0 * (1.0 - b.rail_min / idle));
    CHECK(b.rail_min < idle - 5.0);
    // Half the source resistance: about half the droop.
    OutputStage half = make(0.5);
    const Run c = drive(half, 100.0, 1.0, 4, 10);
    CHECK(c.rail_min > b.rail_min && c.rail_min < idle);
    // Recovery: silence for 10 time constants (tau = Rsup C = 60 ms).
    double in[2] = {0, 0}, out[2];
    const double before = st.rail_voltage();
    for (int n = 0; n < static_cast<int>(0.6 * kFs); ++n) st.tick(in, out);
    std::printf("  recovery: rail %.1f V at the burst's end, %.2f V after 600 ms of silence (idle %.1f V)\n", before,
                st.rail_voltage(), idle);
    CHECK(std::fabs(st.rail_voltage() - idle) < 0.1);
    // Less headroom: the peak of a loud sine is lower sagged than stiff.
    double pk_stiff = 0.0, pk_sag = 0.0;
    for (double y : a.y) pk_stiff = std::fmax(pk_stiff, std::fabs(y));
    for (double y : b.y) pk_sag = std::fmax(pk_sag, std::fabs(y));
    std::printf("  output peak at full-scale 100 Hz: stiff %.4f, sagging %.4f\n", pk_stiff, pk_sag);
    CHECK(pk_sag < pk_stiff);
    return 0;
}

// The solve stays inside its bound and finite on any input, and a NaN or
// infinity is read as silence.
int test_extremes() {
    for (double amp : {1e-6, 1.0, 10.0, 1000.0}) {
        OutputStage st = make(1.0);
        double in[2], out[2];
        for (int n = 0; n < 19200; ++n) {
            const double v = amp * ((n / 600) % 2 == 0 ? 1.0 : -1.0);
            in[0] = v;
            in[1] = std::sin(0.01 * n) * amp;
            st.tick(in, out);
            CHECK(std::isfinite(out[0]) && std::isfinite(out[1]));
        }
        std::printf("  square at %g of full scale: solve <= %u iterations\n", amp, st.max_iterations_seen());
        CHECK(st.max_iterations_seen() <= OutputStage::kMaxIter);
    }
    // A finite input so large that u^4 overflows. Before the cap, |x| of
    // 1e100 or 1e300 made u^4 infinite and the drive voltage rail u / inf = 0:
    // finite, but the stage went silent under the loudest input instead of
    // sitting at the rail; only above about 7.2e305 was it NaN (x * vpu
    // overflowing). So the output is held, sample by sample, to what an
    // input that merely clips (1000 of full scale) gives, which sits at the
    // rail: a 1 kHz square, so the core does not short the drive as it
    // would a held level.
    OutputStage clip = make(1.0);
    std::vector<double> ref;
    double ref_peak = 0.0;
    {
        double cin[2], cout[2];
        for (int n = 0; n < 1920; ++n) {
            cin[0] = cin[1] = 1000.0 * ((n / 96) % 2 == 0 ? 1.0 : -1.0);
            clip.tick(cin, cout);
            ref.push_back(cout[0]);
            if (n >= 960) ref_peak = std::fmax(ref_peak, std::fabs(cout[0]));
        }
    }
    // The clipped peak is the rail's, as a signal level (1.0 = vpu volts at
    // the driver), within what the sag takes off it.
    const double rail_level = (clip.idle_rail_voltage() - 40.0) / 250.0;
    std::printf("  1 kHz square at 1000 of full scale: peak %.4f, the rail at idle %.4f\n", ref_peak, rail_level);
    CHECK(ref_peak > 0.8 * rail_level && ref_peak < 1.05 * rail_level);
    for (double big : {1e100, 1e300, -1e300, 1.7e308}) {
        OutputStage hot = make(1.0);
        double hin[2], hout[2];
        double worst = 0.0;
        for (int n = 0; n < 1920; ++n) {
            const double sq = (n / 96) % 2 == 0 ? 1.0 : -1.0;
            hin[0] = hin[1] = (big > 0.0 ? sq : -sq) * std::fabs(big);
            hot.tick(hin, hout);
            CHECK(std::isfinite(hout[0]) && std::isfinite(hout[1]));
            CHECK(std::isfinite(hot.rail_voltage()) && hot.rail_voltage() >= 10.0);
            worst = std::fmax(worst, std::fabs(std::fabs(hout[0]) - std::fabs(ref[n])));
        }
        std::printf("  square at %g: largest |output| difference from the clipping square %.3g\n", big, worst);
        CHECK_LE(worst, 1e-6 * rail_level, "huge input: output at the rail, as for one that merely clips");
        // Usable afterwards: a 1 kHz tone at 0.1 comes out at 0.1 (the mid
        // band is unity) once the core has settled from the overload.
        double pk = 0.0;
        for (int n = 0; n < 9600; ++n) {
            hin[0] = hin[1] = 0.1 * std::sin(kTwoPi * n / 192.0);
            hot.tick(hin, hout);
            CHECK(std::isfinite(hout[0]) && std::isfinite(hout[1]));
            if (n >= 9600 - 192) pk = std::fmax(pk, std::fabs(hout[0]));
        }
        CHECK(std::fabs(pk - 0.1) < 0.01);
    }
    OutputStage st = make(1.0);
    double in[2] = {std::nan(""), HUGE_VAL}, out[2];
    for (int n = 0; n < 100; ++n) {
        st.tick(in, out);
        CHECK(std::isfinite(out[0]) && std::isfinite(out[1]));
    }
    return 0;
}

// The same circuit at twice the rate gives the same answer: the integration
// converges (the second-order trapezoid, the guarded hysteresis step).
int test_rate_convergence() {
    auto run = [](double fs) {
        OutputStage st;
        st.prepare(fs, 1, {}, {});
        st.set_sag(0.0);
        std::vector<double> y;
        const int per = static_cast<int>(std::lround(fs / 60.0));
        double in[1], out[1];
        for (int n = 0; n < 6 * per; ++n) {
            in[0] = 0.5 * std::sin(kTwoPi * n / per);
            st.tick(in, out);
            if (n >= 5 * per) y.push_back(out[0]);
        }
        return y;
    };
    const std::vector<double> a = run(96000.0), b = run(192000.0), c = run(384000.0);
    double e1 = 0.0, e2 = 0.0, ref = 0.0;
    for (std::size_t n = 0; n < a.size(); ++n) {
        e1 = std::fmax(e1, std::fabs(a[n] - b[2 * n]));
        e2 = std::fmax(e2, std::fabs(b[2 * n] - c[4 * n]));
        ref = std::fmax(ref, std::fabs(c[4 * n]));
    }
    std::printf("  60 Hz at 0.5 FS: max difference 96k vs 192k %.3g, 192k vs 384k %.3g (peak %.3f)\n", e1, e2, ref);
    CHECK(e2 < e1);
    CHECK(e2 < 1e-3 * ref);
    return 0;
}

// A programme-like load (bass, a mid tone and filtered noise under a slow
// envelope) at `gain`, `n` samples at `fs`, channel 1 a variant of 0.
std::vector<double> programme(int n, double fs, double gain, int ch) {
    std::vector<double> x(n);
    std::uint32_t s = 1;
    double lp = 0.0;
    for (int k = 0; k < n; ++k) {
        s = s * 1664525u + 1013904223u;
        lp += 0.05 * (static_cast<double>(s >> 8) / 16777216.0 - 0.5 - lp);
        const double t = k / fs;
        const double env = 0.5 + 0.5 * std::sin(kTwoPi * 0.5 * t);
        const double v = gain * env * (0.5 * std::sin(kTwoPi * 55.0 * t) + 0.3 * std::sin(kTwoPi * 440.0 * t) + 2.0 * lp);
        x[k] = ch == 0 ? v : 0.8 * v + 0.2 * gain * std::sin(kTwoPi * 3000.0 * t);
    }
    return x;
}

// Runs a two-channel stage over x0, x1 one step (`sub` samples) at a time.
void run(OutputStage& st, std::uint32_t sub, const std::vector<double>& x0, const std::vector<double>& x1,
         std::vector<double>& y0, std::vector<double>& y1) {
    y0.assign(x0.size(), 0.0);
    y1.assign(x1.size(), 0.0);
    for (std::size_t k = 0; k + sub <= x0.size(); k += sub) {
        const double* in[2] = {&x0[k], &x1[k]};
        double* out[2] = {&y0[k], &y1[k]};
        st.process(in, out);
    }
}

// The solve at its default tolerance (one evaluation per step, the last
// Newton step along the slope) against the same solve converged to
// -200 dB of full scale, on programme from -18 dBFS to +12 dB over, at
// the rates the tube drive runs it. The mean is the cost bar: a solve
// that needs more evaluations than this is a CPU regression on any
// machine.
int test_solve() {
    const bool quick = dsp_quick();
    double worst_db = -400.0, worst_mean = 0.0, worst_mean_hot = 0.0;
    for (double fs : {48000.0, 96000.0}) {
        for (double gain : {0.125, 0.5, 2.0, 4.0}) {
            const int n = static_cast<int>(fs * (quick ? 1.0 : 4.0));
            const std::vector<double> x0 = programme(n, fs, gain, 0), x1 = programme(n, fs, gain, 1);
            OutputStage fast, tight;
            fast.prepare(fs, 2);
            tight.prepare(fs, 2);
            tight.set_solve_tolerance(1e-10);
            std::vector<double> a0, a1, b0, b1;
            run(fast, 1, x0, x1, a0, a1);
            run(tight, 1, x0, x1, b0, b1);
            double err = 0.0, peak = 0.0;
            for (int k = 0; k < n; ++k) {
                err = std::fmax(err, std::fmax(std::fabs(a0[k] - b0[k]), std::fabs(a1[k] - b1[k])));
                peak = std::fmax(peak, std::fabs(b0[k]));
            }
            const double db = 20.0 * std::log10(err / peak + 1e-300);
            std::printf("  %5.0f Hz, programme x%-5g: error %.1f dB re peak, %.3f evaluations per step (converged: %.3f), at most %u\n",
                        fs, gain, db, fast.mean_iterations(), tight.mean_iterations(), fast.max_iterations_seen());
            worst_db = std::fmax(worst_db, db);
            double& worst = gain <= 0.5 ? worst_mean : worst_mean_hot;
            worst = std::fmax(worst, fast.mean_iterations());
            CHECK(fast.max_iterations_seen() <= OutputStage::kMaxIter);
        }
    }
    // 20 dB under the core's own discretisation at 48 kHz (test_substeps:
    // -55 dB and more against the core at 96 kHz).
    CHECK_LE(worst_db, -90.0, "default solve against the converged one, dB re peak");
    // Into saturation the slope moves most between steps, and at 48 kHz
    // (the tube drive's eco) the predictor misses more often.
    CHECK_LE(worst_mean, 1.02, "core evaluations per step, mean, programme x0.125 and x0.5");
    CHECK_LE(worst_mean_hot, 1.15, "core evaluations per step, mean, programme x2 and x4 (into the rail)");
    return 0;
}

// Two channels as the two lanes of one solve (the default) give the bits
// each gives through the one-channel solve, on programme from quiet to 12 dB
// into the rail, with the core at every sample and once per two: the pair
// is the same arithmetic, not an approximation of it.
int test_pair() {
    for (std::uint32_t sub : {1u, 2u}) {
        for (double gain : {0.125, 2.0, 4.0}) {
            const double fs = 96000.0;
            const int n = static_cast<int>(fs * (dsp_quick() ? 0.5 : 2.0));
            const std::vector<double> x0 = programme(n, fs, gain, 0), x1 = programme(n, fs, gain, 1);
            OutputStage pair, scalar;
            pair.prepare(fs, 2, {}, {}, sub);
            scalar.prepare(fs, 2, {}, {}, sub);
            scalar.set_pair_solve(false);
            std::vector<double> a0, a1, b0, b1;
            run(pair, sub, x0, x1, a0, a1);
            run(scalar, sub, x0, x1, b0, b1);
            std::size_t differ = 0;
            for (int k = 0; k < n; ++k) differ += (a0[k] != b0[k]) + (a1[k] != b1[k]);
            std::printf("  %u substep(s), programme x%g: %zu samples differ, rail %.6f against %.6f V, "
                        "%.3f evaluations per step\n",
                        sub, gain, differ, pair.rail_voltage(), scalar.rail_voltage(), pair.mean_iterations());
            CHECK(differ == 0);
            CHECK(pair.rail_voltage() == scalar.rail_voltage() && pair.flux(1) == scalar.flux(1));
            CHECK(pair.mean_iterations() == scalar.mean_iterations());
        }
    }
    // process_block (the tube drive's call: a block of steps, the sag per
    // base sample) is process() step by step, to the bit, with the sag
    // moving; on two channels (the pair) and on one.
    for (std::uint32_t chs : {2u, 1u}) {
        const double fs = 96000.0;
        const std::uint32_t sub = 2, per_sag = 1, steps = 64;
        const int n = static_cast<int>(steps * sub * 40);
        const std::vector<double> x0 = programme(n, fs, 2.0, 0), x1 = programme(n, fs, 2.0, 1);
        std::vector<double> sag(n / sub);
        for (std::size_t t = 0; t < sag.size(); ++t) sag[t] = 0.5 + 0.5 * std::sin(0.01 * static_cast<double>(t));
        OutputStage blk, one;
        blk.prepare(fs, chs, {}, {}, sub);
        one.prepare(fs, chs, {}, {}, sub);
        std::vector<double> a0(n), a1(n), b0(n), b1(n);
        for (int k = 0; k < n; k += static_cast<int>(steps * sub)) {
            const double* in[2] = {&x0[k], &x1[k]};
            double* out[2] = {&a0[k], &a1[k]};
            blk.process_block(in, out, steps, &sag[k / sub], per_sag);
        }
        for (int k = 0; k < n; k += static_cast<int>(sub)) {
            one.set_sag(sag[k / sub]);
            const double* in[2] = {&x0[k], &x1[k]};
            double* out[2] = {&b0[k], &b1[k]};
            one.process(in, out);
        }
        std::size_t differ = 0;
        for (int k = 0; k < n; ++k) differ += (a0[k] != b0[k]) + (chs == 2 && a1[k] != b1[k]);
        std::printf("  process_block against process, %u channel(s), sag moving: %zu samples differ\n", chs, differ);
        CHECK(differ == 0 && blk.rail_voltage() == one.rail_voltage());
    }
    return 0;
}

// The core stepping once per `sub` samples (the tube drive: once per base
// sample, the clip oversampled): silent at rest, the same circuit to
// within the discretisation as the core stepping every sample, and the
// physics the suite holds above (unity mid band, LF first) unchanged.
int test_substeps() {
    {
        OutputStage st;
        CHECK(!st.prepare(96000.0, 2, {}, {}, 0) && !st.prepare(96000.0, 2, {}, {}, OutputStage::kMaxSubsteps + 1));
        CHECK(st.prepare(96000.0, 2, {}, {}, 2));
        std::vector<double> z(960, 0.0), y0, y1;
        run(st, 2, z, z, y0, y1);
        for (std::size_t k = 0; k < z.size(); ++k) CHECK(y0[k] == 0.0 && y1[k] == 0.0);
    }
    const double base = 48000.0;
    for (std::uint32_t sub : {2u, 4u, 8u}) {
        const double fs = base * sub;
        double thd_at[2];
        const double freqs[2] = {1000.0, 40.0};
        for (int f = 0; f < 2; ++f) {
            const int per = static_cast<int>(std::lround(fs / freqs[f]));
            const int n = 14 * per;
            std::vector<double> x(n);
            for (int k = 0; k < n; ++k) x[k] = 0.5 * std::sin(kTwoPi * k / per);
            OutputStage coarse, fine;
            coarse.prepare(fs, 2, {}, {}, sub);
            coarse.set_sag(0.0);
            fine.prepare(fs, 2, {}, {}, 1);
            fine.set_sag(0.0);
            std::vector<double> a0, a1, b0, b1;
            run(coarse, sub, x, x, a0, a1);
            run(fine, 1, x, x, b0, b1);
            const std::vector<double> tail(a0.begin() + 6 * per, a0.end());
            double h1 = 0.0, err = 0.0;
            thd_at[f] = thd(tail, per, &h1);
            for (int k = 6 * per; k < n; ++k) err = std::fmax(err, std::fabs(a0[k] - b0[k]));
            std::printf("  core at %.0f Hz, %u substeps, %4.0f Hz at 0.5 FS: gain %.3f dB, THD %.3f %%, "
                        "largest difference from the core at %.0f Hz %.3g\n",
                        base, sub, freqs[f], 20.0 * std::log10(h1 / 0.5), 100.0 * thd_at[f], fs, err);
            if (f == 0) CHECK(std::fabs(20.0 * std::log10(h1 / 0.5)) < 0.5);
            // The bass loop at a step of 1/48 kHz: within 1 % of full scale
            // of the core at the full rate.
            CHECK_LE(err, 0.01, "core at the base rate against the full rate, 0.5 FS");
        }
        CHECK_LE(thd_at[0], 0.02, "THD at 1 kHz, 0.5 FS, core at the base rate");
        CHECK(thd_at[1] > 3.0 * thd_at[0]);
    }
    return 0;
}

}  // namespace

int main() {
    int fails = 0;
    std::printf("rest\n");
    fails += test_rest();
    std::printf("response at low level\n");
    fails += test_low_level_response();
    std::printf("mid band, LF first\n");
    fails += test_mid_band_and_lf_first();
    std::printf("flux\n");
    fails += test_flux_follows_one_over_f();
    std::printf("hysteresis\n");
    fails += test_hysteresis_dissipates();
    std::printf("sag\n");
    fails += test_sag();
    std::printf("extremes\n");
    fails += test_extremes();
    std::printf("rate convergence\n");
    fails += test_rate_convergence();
    std::printf("solve\n");
    fails += test_solve();
    std::printf("two channels in lockstep\n");
    fails += test_pair();
    std::printf("core at the base rate\n");
    fails += test_substeps();
    return fails == 0 ? 0 : 1;
}
