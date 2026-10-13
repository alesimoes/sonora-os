// SPDX-License-Identifier: GPL-3.0-or-later
//
// Low-frequency oscillator for modulated delays. The phase is a double
// accumulator in [0, 1), so a voice's phase offset is exact and the rate can
// change every sample without a discontinuity in the output.
#pragma once

#include <cmath>

#include "sonora/dsp/math.h"
#include "sonora/rt.h"

namespace sonora::dsp {

enum class LfoShape { Sine, Triangle };

class Lfo {
public:
    void reset(double phase = 0.0) noexcept { phase_ = phase - std::floor(phase); }
    void set_rate(double hz, double sample_rate) noexcept {
        inc_ = sample_rate > 0.0 ? hz / sample_rate : 0.0;
    }
    // Value in [-1, 1] at this sample's phase plus `offset` cycles.
    static double shape_at(LfoShape shape, double phase) noexcept SONORA_RT {
        phase -= std::floor(phase);
        if (shape == LfoShape::Sine) return std::sin(kTwoPi * phase);
        // Triangle starting at 0 and rising, in phase with the sine.
        const double p = phase + 0.25;
        const double f = p - std::floor(p);
        return 1.0 - 4.0 * std::fabs(f - 0.5);
    }
    double value(LfoShape shape, double offset = 0.0) const noexcept SONORA_RT {
        return shape_at(shape, phase_ + offset);
    }
    void advance() noexcept SONORA_RT {
        phase_ += inc_;
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);
    }
    double phase() const noexcept { return phase_; }

private:
    double phase_ = 0.0;
    double inc_ = 0.0;
};

}  // namespace sonora::dsp
