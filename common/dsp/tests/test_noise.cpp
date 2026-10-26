// SPDX-License-Identifier: GPL-3.0-or-later
//
// The noise and hum generators (noise.h) and the power supply's ripple
// (supply.h): invariants that need nothing but common/dsp. The levels and
// spectra against ngspice are the tube processor's suite
// (plugins/native/tube/tests/suite.cpp), which reads the committed SPICE
// reference (tests/spice-noise/).
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/noise.h"
#include "sonora/dsp/supply.h"

namespace {

using namespace sonora::dsp;

// The sequence is pinned: golden files depend on it, so a change to the
// generator must fail here first. The values are xoshiro256** seeded by
// splitmix64 from 1, computed independently (Python integers).
int test_rng_sequence() {
    NoiseRng r(1);
    CHECK(r.next() == 0xb3f2af6d0fc710c5ull);
    CHECK(r.next() == 0x853b559647364ceaull);
    CHECK(r.next() == 0x92f89756082a4514ull);
    // Reseeding restarts it; another seed is another sequence.
    r.seed_with(1);
    CHECK(r.next() == 0xb3f2af6d0fc710c5ull);
    NoiseRng z(0), o(2);
    CHECK(z.next() != 0 && o.next() != 0xb3f2af6d0fc710c5ull);
    return 0;
}

// Unit variance, zero mean, the Irwin-Hall (n = 8) kurtosis, bounded.
int test_gaussian_moments() {
    GaussianNoise g(7);
    const std::uint32_t n = 1u << 22;
    double m1 = 0.0, m2 = 0.0, m4 = 0.0, peak = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const double x = g.next();
        m1 += x;
        m2 += x * x;
        m4 += x * x * x * x;
        peak = std::fmax(peak, std::fabs(x));
    }
    m1 /= n;
    m2 /= n;
    m4 /= n;
    const double kurt = m4 / (m2 * m2);
    std::printf("  gaussian: mean %.2e, variance %.5f, kurtosis %.4f (Irwin-Hall n=8: 2.85), peak %.2f\n", m1, m2,
                kurt, peak);
    CHECK_LE(std::fabs(m1), 2e-3, "mean");
    CHECK_LE(std::fabs(m2 - 1.0), 3e-3, "variance - 1");
    CHECK_LE(std::fabs(kurt - 2.85), 0.01, "kurtosis - 2.85");
    CHECK_LE(peak, 4.9, "peak");
    return 0;
}

// White: the power in 16 equal bands of the spectrum, averaged over 2^22
// samples, within 0.05 dB of each other's mean (the estimate's own
// spread is about 0.012 dB).
int test_gaussian_white() {
    const std::uint32_t size = 4096, frames = 1024;
    Fft fft;
    CHECK(fft.prepare(size));
    std::vector<double> band(16, 0.0);
    std::vector<std::complex<double>> buf(size);
    GaussianNoise g(11);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (auto& b : buf) b = g.next();
        fft.forward(buf.data());
        for (std::uint32_t k = 1; k < size / 2; ++k) band[k * 16 / (size / 2)] += std::norm(buf[k]);
    }
    double mean = 0.0;
    for (double b : band) mean += b / 16.0;
    double worst = 0.0;
    for (double b : band) worst = std::fmax(worst, std::fabs(10.0 * std::log10(b / mean)));
    std::printf("  gaussian: band powers within %.4f dB of their mean\n", worst);
    CHECK_LE(worst, 0.05, "band flatness, dB");
    return 0;
}

// The hum bank against direct cosines after 60 s, renormalised once per
// 128-sample block as the processor does: phase and amplitude hold.
int test_hum_bank() {
    const double fs = 48000.0;
    const std::vector<double> f = {100.0, 200.0, 300.0, 30000.0};
    const std::vector<std::complex<double>> a = {{1e-3, 2e-4}, {-3e-4, 1e-4}, {0.0, -5e-5}, {1.0, 0.0}};
    HumBank h;
    CHECK(h.prepare(fs, f, a) == 3);  // 30 kHz is past Nyquist
    const std::uint64_t n = static_cast<std::uint64_t>(60.0 * fs);
    double worst = 0.0;
    for (std::uint64_t i = 0; i < n; ++i) {
        const double y = h.next();
        if ((i + 1) % 128 == 0) h.renormalise();
        if (i + 48000 >= n) {
            const double t = static_cast<double>(i) / fs;
            double want = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                const double ph = kTwoPi * std::fmod(f[k] * t, 1.0);
                want += a[k].real() * std::cos(ph) - a[k].imag() * std::sin(ph);
            }
            worst = std::fmax(worst, std::fabs(y - want));
        }
    }
    std::printf("  hum bank after 60 s: max |error| %.3g (largest partial 1e-3)\n", worst);
    CHECK_LE(worst, 1e-11, "hum bank error");
    h.reset();
    CHECK(std::fabs(h.next() - (1e-3 - 3e-4)) <= 1e-18);
    return 0;
}

// The supply settles, lands its node near the stage's 250 V, and its
// ripple is the reservoir's: the fundamental near the sawtooth estimate
// I / (2 f C) / pi, and the harmonics converged in the step.
int test_supply() {
    for (double mains : {50.0, 60.0}) {
        SupplyCircuit c;
        c.mains_hz = mains;
        SupplyRipple r, fine;
        CHECK(simulate_supply(c, 8, r));
        CHECK(simulate_supply(c, 8, fine, 16000));
        const double est = (c.i_output + c.i_preamp) / (2.0 * mains * c.c_reservoir) / kPi;
        std::printf("  supply %g Hz: reservoir %.2f V DC, node %.2f V, ripple H1 %.4f V (sawtooth estimate %.4f), "
                    "H2 %.2f dBc, Newton at most %u\n",
                    mains, r.reservoir_dc, r.node_dc, std::abs(r.reservoir[0]), est,
                    20.0 * std::log10(std::abs(r.reservoir[1]) / std::abs(r.reservoir[0])), r.max_newton);
        CHECK_LE(std::fabs(r.node_dc - 250.0), 5.0, "node DC - 250 V");
        CHECK_LE(std::fabs(std::abs(r.reservoir[0]) / est - 1.0), 0.25, "H1 against the sawtooth estimate");
        CHECK_LE(r.max_newton, 60, "Newton iterations");
        double worst = 0.0;
        for (std::uint32_t k = 0; k < 5; ++k)
            worst = std::fmax(worst, std::abs(r.reservoir[k] - fine.reservoir[k]) / std::abs(r.reservoir[0]));
        std::printf("  supply %g Hz: 4000 against 16000 steps a period, max |dH| / |H1| over 5 harmonics %.3g\n",
                    mains, worst);
        CHECK_LE(worst, 1e-3, "step convergence");
    }
    return 0;
}

// Two equal RC sections, unloaded: 1 / (1 + 3 sRC + (sRC)^2).
int test_filter_transfer() {
    SupplyCircuit c;
    const double rc = c.r_filter1 * c.c_filter1;
    double worst = 0.0;
    for (double f : {1.0, 100.0, 120.0, 1000.0, 10000.0}) {
        const std::complex<double> s(0.0, kTwoPi * f * rc);
        const std::complex<double> want = 1.0 / (1.0 + 3.0 * s + s * s);
        worst = std::fmax(worst, std::abs(supply_filter_transfer(c, f, 0.0) / want - 1.0));
    }
    CHECK_LE(worst, 1e-12, "filter transfer, relative");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_rng_sequence();
    failed += test_gaussian_moments();
    failed += test_gaussian_white();
    failed += test_hum_bank();
    failed += test_supply();
    failed += test_filter_transfer();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp noise: all passed\n");
    return 0;
}
