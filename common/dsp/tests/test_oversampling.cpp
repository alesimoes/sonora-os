// SPDX-License-Identifier: GPL-3.0-or-later
//
// The oversampler's latency is measured, not trusted (ADR-0011 §1.5), its
// passband is flat, and it does what it is for: a nonlinearity run inside
// it aliases far less.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"

namespace {

using namespace sonora::dsp;

const double kRates[] = {44100.0, 48000.0, 96000.0};
const std::uint32_t kFactors[] = {1, 2, 4, 8};

// Runs `in` through up() and down() in blocks, applying `shape` to every
// oversampled sample.
template <class F>
std::vector<float> round_trip(Oversampler& os, const std::vector<float>& in,
                              std::uint32_t block, F shape) {
    std::vector<float> out(in.size());
    for (std::size_t pos = 0; pos < in.size(); pos += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, in.size() - pos));
        double* hi = os.up(0, &in[pos], n);
        for (std::uint32_t i = 0; i < n * os.factor(); ++i) hi[i] = shape(hi[i]);
        os.down(0, &out[pos], n);
    }
    return out;
}

int test_latency_is_exact() {
    for (double fs : kRates) {
        for (std::uint32_t f : kFactors) {
            Oversampler os;
            CHECK(os.prepare(f, 1, 128));
            std::vector<float> in(4096, 0.0f);
            in[100] = 1.0f;
            const auto out = round_trip(os, in, 128, [](double x) { return x; });
            std::size_t peak = 0;
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (std::fabs(out[i]) > std::fabs(out[peak])) peak = i;
            }
            const long measured = static_cast<long>(peak) - 100;
            std::printf("oversampling %ux at %g: reported %u, measured %ld, peak %.6f\n", f, fs,
                        os.latency(), measured, out[peak]);
            CHECK(measured == static_cast<long>(os.latency()));
            // A linear-phase, unit-gain path: the impulse comes back whole.
            CHECK(std::fabs(out[peak] - 1.0f) < 0.02f);
        }
    }
    return 0;
}

double sine_gain_db(Oversampler& os, double fs, double freq) {
    const std::size_t n = 16384;
    std::vector<float> in(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = static_cast<float>(0.5 * std::sin(kTwoPi * freq * i / fs));
    const auto out = round_trip(os, in, 128, [](double x) { return x; });
    // Compare RMS over the second half, past the filter's warm-up.
    double ei = 0.0, eo = 0.0;
    for (std::size_t i = n / 2; i < n; ++i) {
        ei += in[i - os.latency()] * in[i - os.latency()];
        eo += out[i] * out[i];
    }
    return 10.0 * std::log10(eo / ei);
}

int test_passband_is_flat() {
    for (double fs : kRates) {
        for (std::uint32_t f : {2u, 4u, 8u}) {
            Oversampler os;
            CHECK(os.prepare(f, 1, 128));
            double worst = 0.0;
            for (double freq : {50.0, 1000.0, 10000.0, 18000.0, 20000.0}) {
                worst = std::fmax(worst, std::fabs(sine_gain_db(os, fs, freq)));
                os.reset();
            }
            CHECK_LE(worst, 0.01, "passband deviation (dB)");
        }
    }
    return 0;
}

// Energy at frequencies that are not harmonics of f0, relative to the
// fundamental: what a nonlinearity folds back into the band.
double alias_dbc(const std::vector<float>& y, double fs, double f0) {
    const std::uint32_t n = 32768;
    Fft fft;
    fft.prepare(n);
    std::vector<std::complex<double>> x(n);
    const std::size_t start = y.size() - n;
    for (std::uint32_t i = 0; i < n; ++i) {
        // Blackman-Harris 4-term window: sidelobes under -92 dB.
        const double t = kTwoPi * i / n;
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) -
                         0.01168 * std::cos(3 * t);
        x[i] = y[start + i] * w;
    }
    fft.forward(x.data());
    double fund = 0.0, other = 0.0;
    for (std::uint32_t k = 1; k < n / 2; ++k) {
        const double f = k * fs / n;
        const double p = std::norm(x[k]);
        const double h = f / f0;
        const double nearest = std::round(h);
        const bool harmonic = nearest >= 1.0 && std::fabs(f - nearest * f0) < 6.0 * fs / n;
        if (harmonic && nearest == 1.0) fund += p;
        else if (!harmonic) other += p;
    }
    return 10.0 * std::log10(other / fund);
}

int test_oversampling_reduces_aliasing() {
    // A hard clipper on a 9 kHz sine at 48 kHz: the 3rd harmonic (27 kHz)
    // folds to 21 kHz at 1x. Inside 4x it is removed before decimation.
    const double fs = 48000.0, f0 = 9000.0;
    std::vector<float> in(65536);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(0.9 * std::sin(kTwoPi * f0 * i / fs));
    auto clip = [](double x) { return std::fmax(-0.5, std::fmin(0.5, x)); };
    double prev = 0.0;
    for (std::uint32_t f : kFactors) {
        Oversampler os;
        CHECK(os.prepare(f, 1, 128));
        const double a = alias_dbc(round_trip(os, in, 128, clip), fs, f0);
        std::printf("hard clip, 9 kHz at 48 kHz, %ux: non-harmonic energy %.1f dBc\n", f, a);
        if (f > 1) CHECK(a < prev);
        prev = a;
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_latency_is_exact();
    failed += test_passband_is_flat();
    failed += test_oversampling_reduces_aliasing();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp oversampling: all passed\n");
    return 0;
}
