// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/noise.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

void NoiseRng::seed_with(std::uint64_t seed) noexcept {
    // splitmix64 (S. Vigna, after G. Steele, D. Lea and C. Flood, OOPSLA
    // 2014): fills the state so that no seed, 0 included, gives the
    // all-zero state xoshiro cannot leave.
    std::uint64_t x = seed;
    for (auto& s : s_) {
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        s = z ^ (z >> 31);
    }
}

std::uint32_t HumBank::prepare(double sample_rate, const std::vector<double>& freq_hz,
                               const std::vector<std::complex<double>>& amplitude) {
    n_ = 0;
    const std::size_t count = freq_hz.size() < amplitude.size() ? freq_hz.size() : amplitude.size();
    for (std::size_t k = 0; k < count && n_ < kMaxPartials; ++k) {
        if (!(freq_hz[k] > 0.0) || !(freq_hz[k] < 0.5 * sample_rate)) continue;
        const double w = kTwoPi * freq_hz[k] / sample_rate;
        wr_[n_] = std::cos(w);
        wi_[n_] = std::sin(w);
        cr_[n_] = amplitude[k].real();
        ci_[n_] = amplitude[k].imag();
        ++n_;
    }
    reset();
    return n_;
}

void HumBank::reset() noexcept {
    for (std::uint32_t k = 0; k < kMaxPartials; ++k) {
        ur_[k] = 1.0;
        ui_[k] = 0.0;
    }
}

void HumBank::renormalise() noexcept {
    for (std::uint32_t k = 0; k < n_; ++k) {
        const double m = ur_[k] * ur_[k] + ui_[k] * ui_[k];
        const double g = 1.5 - 0.5 * m;
        ur_[k] *= g;
        ui_[k] *= g;
    }
}

}  // namespace sonora::dsp
