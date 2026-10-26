// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeded noise and a bank of hum oscillators: the generators behind the
// modelled noise and hum of the analog processors (ADR-0011 §3.3: "each
// is generated from a seeded generator, so golden files stay
// deterministic"; "at zero it is digitally silent"). Stage N2, slice 5.
//
// What the levels and spectra are is not decided here: the circuit that
// makes them decides (triode.h, small_signal_noise(); supply.h, the
// rectifier's ripple). These are the sources those figures drive.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp {

// xoshiro256** (D. Blackman and S. Vigna, "Scrambled linear pseudorandom
// number generators", ACM TOMS 47(4), 2021), seeded through splitmix64 as
// its authors recommend. Integer arithmetic only, so the sequence is the
// same on every compiler and architecture: the golden files depend on it.
class NoiseRng {
public:
    explicit NoiseRng(std::uint64_t seed = 1) noexcept { seed_with(seed); }
    void seed_with(std::uint64_t seed) noexcept;
    std::uint64_t next() noexcept SONORA_RT {
        const std::uint64_t r = rotl(s_[1] * 5u, 7) * 9u;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return r;
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4] = {};
};

// Zero-mean, unit-variance noise, close to Gaussian: the sum of eight
// uniform 16-bit values from two draws, centred and scaled (the
// Irwin-Hall distribution, n = 8). Its kurtosis is 2.85 against the normal
// distribution's 3, and it is bounded at +-4.9 sigma; thermal and shot
// noise are Gaussian, but at the levels the models produce (100 dB and
// more under full scale) neither difference is measurable, and no libm
// call means no compiler-dependent bits and a few nanoseconds a sample.
class GaussianNoise {
public:
    explicit GaussianNoise(std::uint64_t seed = 1) noexcept : rng_(seed) {}
    void seed_with(std::uint64_t seed) noexcept { rng_.seed_with(seed); }
    double next() noexcept SONORA_RT {
        const std::uint64_t a = rng_.next(), b = rng_.next();
        std::uint64_t sum = 0;
        for (int k = 0; k < 64; k += 16) sum += ((a >> k) & 0xFFFFu) + ((b >> k) & 0xFFFFu);
        return (static_cast<double>(sum) - kMean) * kInvSigma;
    }

private:
    // Eight values uniform on {0 .. 65535}: mean 8 x 32767.5, variance
    // 8 x (65536^2 - 1) / 12.
    static constexpr double kMean = 8.0 * 32767.5;
    static constexpr double kInvSigma = 1.0 / 53509.91992145008;  // 1 / sqrt(8 (65536^2 - 1) / 12)
    NoiseRng rng_;
};

// A sum of sinusoids at fixed frequencies, complex amplitudes and a common
// time origin: the supply's hum at the output of a stage. Each partial is
// a unit phasor rotated once a sample (four multiplies, not a sine call)
// and renormalised once a block, so its amplitude does not drift over
// hours. Sized in prepare(); next() allocates nothing.
class HumBank {
public:
    static constexpr std::uint32_t kMaxPartials = 32;
    // Not real-time. Each partial: frequency (Hz) and complex amplitude,
    // y(t) = sum Re(c e^{j 2 pi f t}). Partials past kMaxPartials, or at
    // or above Nyquist, are dropped. Returns how many were kept.
    std::uint32_t prepare(double sample_rate, const std::vector<double>& freq_hz,
                          const std::vector<std::complex<double>>& amplitude);
    // Back to t = 0.
    void reset() noexcept SONORA_RT;
    double next() noexcept SONORA_RT {
        double y = 0.0;
        for (std::uint32_t k = 0; k < n_; ++k) {
            const double ur = ur_[k] * wr_[k] - ui_[k] * wi_[k];
            const double ui = ur_[k] * wi_[k] + ui_[k] * wr_[k];
            y += cr_[k] * ur_[k] - ci_[k] * ui_[k];
            ur_[k] = ur;
            ui_[k] = ui;
        }
        return y;
    }
    // Pulls each phasor back to unit length (one Newton step on |u| = 1):
    // the rounding of the rotations otherwise grows or shrinks it by about
    // 1e-16 a sample. Call once a block.
    void renormalise() noexcept SONORA_RT;
    std::uint32_t partials() const noexcept { return n_; }

private:
    std::uint32_t n_ = 0;
    double wr_[kMaxPartials] = {}, wi_[kMaxPartials] = {};  // e^{j w T}
    double ur_[kMaxPartials] = {}, ui_[kMaxPartials] = {};  // e^{j w t}
    double cr_[kMaxPartials] = {}, ci_[kMaxPartials] = {};  // amplitude
};

}  // namespace sonora::dsp
