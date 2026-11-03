// SPDX-License-Identifier: GPL-3.0-or-later
//
// Radix-2 complex FFT (Cooley-Tukey, iterative, in place). The twiddles and
// the bit-reversal table are built in prepare(); transform() allocates
// nothing. Used by the measurement harness and, later, by the UI's
// analysers (ADR-0011 §2.6), so a curve and a measurement come from the
// same code.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp {

class Fft {
public:
    // size must be a power of two. Allocates.
    bool prepare(std::uint32_t size);
    std::uint32_t size() const noexcept { return n_; }
    // Forward: X[k] = sum x[n] e^{-j 2 pi k n / N}. Inverse divides by N.
    void forward(std::complex<double>* data) const noexcept SONORA_RT;
    void inverse(std::complex<double>* data) const noexcept SONORA_RT;

private:
    void run(std::complex<double>* data, bool inverse) const noexcept;
    std::uint32_t n_ = 0;
    std::vector<std::complex<double>> twiddle_;
    std::vector<std::uint32_t> rev_;
};

// The transform of a real sequence of n points through one complex
// transform of n / 2 (the even samples as real parts, the odd ones as
// imaginary parts, separated after by the conjugate symmetry): half the
// work of a complex transform of n with a zero imaginary part. E. O.
// Brigham, *The Fast Fourier Transform and its Applications*, 1988, §9-3.
// Used by the convolver (convolver.h), whose blocks are real.
class RealFft {
public:
    // n must be a power of two, at least 4. Allocates.
    bool prepare(std::uint32_t n);
    std::uint32_t size() const noexcept { return n_; }
    // x: n real points. X: bins 0 .. n / 2, the rest being their conjugates.
    void forward(const double* x, std::complex<double>* X) noexcept SONORA_RT;
    // The inverse of forward(), divided by n: x from X[0 .. n / 2], whose
    // bins 0 and n / 2 are taken as real.
    void inverse(const std::complex<double>* X, double* x) noexcept SONORA_RT;

private:
    std::uint32_t n_ = 0;
    Fft half_;
    std::vector<std::complex<double>> w_;  // e^{-j 2 pi k / n}, k = 0 .. n / 2
    std::vector<std::complex<double>> z_;  // n / 2
};

// Kaiser window value for |x| <= 1 (x = position / half-length), and the
// zeroth-order modified Bessel function it needs. J. F. Kaiser, "Nonrecursive
// digital filter design using the I0-sinh window function", Proc. IEEE
// ISCAS, 1974.
double bessel_i0(double x) noexcept;
double kaiser(double x, double beta) noexcept;
// beta for a stopband attenuation in dB (Kaiser's empirical formula).
double kaiser_beta(double attenuation_db) noexcept;

}  // namespace sonora::dsp
