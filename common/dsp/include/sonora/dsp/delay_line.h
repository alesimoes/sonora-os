// SPDX-License-Identifier: GPL-3.0-or-later
//
// Circular delay line with integer and fractional reads.
//
// Fractional reads use third-order Lagrange interpolation (T. I. Laakso,
// V. Valimaki, M. Karjalainen, U. K. Laine, "Splitting the unit delay",
// IEEE Signal Processing Magazine 13(1), 1996, §III.B), evaluated with the
// fractional part placed in [1, 2) where its magnitude and phase error are
// smallest. Allpass interpolation is not used for modulated delays: its
// state carries the old delay into the new one (J. Dattorro, "Effect Design
// Part 2", JAES 45(10), 1997).
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp {

class DelayLine {
public:
    // Allocates: call from prepare(). Holds at least max_delay + 4 samples.
    void prepare(std::uint32_t max_delay) {
        std::uint32_t size = 1;
        while (size < max_delay + 4) size <<= 1;
        buf_.assign(size, 0.0f);
        mask_ = size - 1;
        pos_ = 0;
    }
    void reset() noexcept {
        for (float& x : buf_) x = 0.0f;
        pos_ = 0;
    }
    std::uint32_t capacity() const noexcept { return mask_ + 1 - 4; }

    void push(float x) noexcept SONORA_RT {
        pos_ = (pos_ + 1) & mask_;
        buf_[pos_] = x;
    }
    // delay 0 is the sample pushed last.
    float read(std::uint32_t delay) const noexcept SONORA_RT {
        return buf_[(pos_ - delay) & mask_];
    }
    float read_linear(double delay) const noexcept SONORA_RT {
        const double fl = std::floor(delay);
        const auto i = static_cast<std::uint32_t>(fl);
        const double f = delay - fl;
        return static_cast<float>((1.0 - f) * read(i) + f * read(i + 1));
    }
    // delay >= 1.
    float read_lagrange3(double delay) const noexcept SONORA_RT {
        const double fl = std::floor(delay);
        const auto base = static_cast<std::uint32_t>(fl) - 1u;
        const double d = delay - fl + 1.0;  // in [1, 2)
        const double dm1 = d - 1.0, dm2 = d - 2.0, dm3 = d - 3.0;
        const double h0 = -dm1 * dm2 * dm3 / 6.0;
        const double h1 = d * dm2 * dm3 / 2.0;
        const double h2 = -d * dm1 * dm3 / 2.0;
        const double h3 = d * dm1 * dm2 / 6.0;
        return static_cast<float>(h0 * read(base) + h1 * read(base + 1) +
                                  h2 * read(base + 2) + h3 * read(base + 3));
    }

private:
    std::vector<float> buf_;
    std::uint32_t mask_ = 0;
    std::uint32_t pos_ = 0;
};

}  // namespace sonora::dsp
