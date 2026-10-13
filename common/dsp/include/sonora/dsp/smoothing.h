// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parameter smoothing (ADR-0011 §2.4: "Smoothing is mandatory").
//
// A linear ramp reaches its target in a known number of samples, so a
// parameter change has a bounded, testable duration and the zipper test can
// say when it is over. The exponential one-pole in dynamics.h is for
// detectors, where the time constant is the point.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::dsp {

class LinearSmoother {
public:
    // Ramp length for every later set_target(). Zero jumps.
    void set_ramp_samples(std::uint32_t n) noexcept { ramp_ = n; }
    std::uint32_t ramp_samples() const noexcept { return ramp_; }

    // Jump, with no ramp: for prepare() and state restore.
    void reset(double value) noexcept {
        current_ = target_ = value;
        increment_ = 0.0;
        left_ = 0;
    }

    void set_target(double target) noexcept SONORA_RT {
        target_ = target;
        if (ramp_ == 0 || target_ == current_) {
            current_ = target_;
            left_ = 0;
            return;
        }
        left_ = ramp_;
        increment_ = (target_ - current_) / static_cast<double>(ramp_);
    }

    double next() noexcept SONORA_RT {
        if (left_ != 0) {
            --left_;
            // The last step lands exactly on the target, so rounding in the
            // increments never leaves a residue that reads as "still moving".
            current_ = left_ == 0 ? target_ : current_ + increment_;
        }
        return current_;
    }

    // Moves n samples along the ramp at once (control-rate consumers).
    double advance(std::uint32_t n) noexcept SONORA_RT {
        if (left_ == 0) return current_;
        if (n >= left_) {
            left_ = 0;
            current_ = target_;
        } else {
            left_ -= n;
            current_ += increment_ * static_cast<double>(n);
        }
        return current_;
    }

    double current() const noexcept { return current_; }
    double target() const noexcept { return target_; }
    bool moving() const noexcept { return left_ != 0; }

private:
    double current_ = 0.0;
    double target_ = 0.0;
    double increment_ = 0.0;
    std::uint32_t left_ = 0;
    std::uint32_t ramp_ = 0;
};

}  // namespace sonora::dsp
