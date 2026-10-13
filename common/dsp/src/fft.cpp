// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/fft.h"

#include <cmath>
#include <utility>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

bool Fft::prepare(std::uint32_t size) {
    if (size < 2 || (size & (size - 1)) != 0) return false;
    n_ = size;
    twiddle_.resize(size / 2);
    for (std::uint32_t k = 0; k < size / 2; ++k) {
        twiddle_[k] = std::polar(1.0, -kTwoPi * k / size);
    }
    rev_.resize(size);
    std::uint32_t bits = 0;
    while ((1u << bits) < size) ++bits;
    for (std::uint32_t i = 0; i < size; ++i) {
        std::uint32_t r = 0;
        for (std::uint32_t b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
        rev_[i] = r;
    }
    return true;
}

void Fft::run(std::complex<double>* x, bool inverse) const noexcept {
    for (std::uint32_t i = 0; i < n_; ++i) {
        if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
    }
    for (std::uint32_t len = 2; len <= n_; len <<= 1) {
        const std::uint32_t half = len / 2;
        const std::uint32_t step = n_ / len;
        for (std::uint32_t start = 0; start < n_; start += len) {
            for (std::uint32_t k = 0; k < half; ++k) {
                std::complex<double> w = twiddle_[k * step];
                if (inverse) w = std::conj(w);
                const std::complex<double> t = w * x[start + k + half];
                x[start + k + half] = x[start + k] - t;
                x[start + k] += t;
            }
        }
    }
    if (inverse) {
        const double s = 1.0 / n_;
        for (std::uint32_t i = 0; i < n_; ++i) x[i] *= s;
    }
}

void Fft::forward(std::complex<double>* data) const noexcept { run(data, false); }
void Fft::inverse(std::complex<double>* data) const noexcept { run(data, true); }

double bessel_i0(double x) noexcept {
    // Power series; converges quickly for the betas a filter design uses.
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

double kaiser(double x, double beta) noexcept {
    if (x < -1.0 || x > 1.0) return 0.0;
    return bessel_i0(beta * std::sqrt(1.0 - x * x)) / bessel_i0(beta);
}

double kaiser_beta(double a) noexcept {
    if (a > 50.0) return 0.1102 * (a - 8.7);
    if (a >= 21.0) return 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    return 0.0;
}

}  // namespace sonora::dsp
