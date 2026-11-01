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
    // A finite input so large that u^4 overflows: the drive voltage, the
    // core and the rail stay finite, and the stage works afterwards.
    for (double big : {1e100, 1e300, -1e300, 1.7e308}) {
        OutputStage hot = make(1.0);
        double hin[2] = {big, -big}, hout[2];
        for (int n = 0; n < 2000; ++n) {
            hot.tick(hin, hout);
            CHECK(std::isfinite(hout[0]) && std::isfinite(hout[1]));
            CHECK(std::isfinite(hot.rail_voltage()) && hot.rail_voltage() >= 10.0);
        }
        hin[0] = hin[1] = 0.1;
        for (int n = 0; n < 4800; ++n) hot.tick(hin, hout);
        CHECK(std::isfinite(hout[0]) && std::fabs(hout[0]) > 0.01);
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
    return fails == 0 ? 0 : 1;
}
