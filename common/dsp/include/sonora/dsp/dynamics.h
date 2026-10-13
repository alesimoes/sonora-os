// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gain computers and level detectors for compressors, expanders and gates,
// after D. Giannoulis, M. Massberg and J. D. Reiss, "Digital dynamic range
// compressor design -- a tutorial and analysis", JAES 60(6), 2012,
// pp. 399-408.
//
// The topology the paper recommends, and the one the native processors use,
// is log domain throughout: level in dB -> static gain computer -> gain
// reduction smoothed by a branching peak detector (their eq. 17) -> gain.
// Smoothing the gain reduction rather than the input level gives attack and
// release that do not depend on threshold or ratio, which is why the attack
// and release a processor displays can be measured and held to +-10 %
// (ADR-0011 §3.2).
#pragma once

#include <cmath>

#include "sonora/dsp/math.h"
#include "sonora/rt.h"

namespace sonora::dsp {

// Downward compressor with a quadratic soft knee (Giannoulis et al. eq. 4).
// Returns the output level in dB for an input level x_db.
inline double compressor_curve_db(double x_db, double threshold_db, double ratio,
                                  double knee_db) noexcept {
    const double over = x_db - threshold_db;
    if (2.0 * over < -knee_db) return x_db;
    if (knee_db > 0.0 && 2.0 * std::fabs(over) <= knee_db) {
        const double t = over + 0.5 * knee_db;
        return x_db + (1.0 / ratio - 1.0) * t * t / (2.0 * knee_db);
    }
    return threshold_db + over / ratio;
}

// Downward expander below threshold, the mirror image of the compressor
// curve: y = T + (x - T) * ratio under the knee, with the same quadratic
// blend so the slope goes continuously from 1 to `ratio`.
inline double expander_curve_db(double x_db, double threshold_db, double ratio,
                                double knee_db) noexcept {
    const double under = x_db - threshold_db;
    if (2.0 * under > knee_db) return x_db;
    if (knee_db > 0.0 && 2.0 * std::fabs(under) <= knee_db) {
        const double t = under - 0.5 * knee_db;
        return x_db - (ratio - 1.0) * t * t / (2.0 * knee_db);
    }
    return threshold_db + under * ratio;
}

// Smooth branching peak detector (Giannoulis et al. eq. 17) on a
// non-negative control signal: rises with the attack coefficient, falls
// with the release one. Each branch is an exact one-pole, so a step reaches
// 63 % in exactly the set time.
class BranchingSmoother {
public:
    void set_times(double attack_s, double release_s, double sample_rate) noexcept {
        attack_ = time_constant_coeff(attack_s, sample_rate);
        release_ = time_constant_coeff(release_s, sample_rate);
    }
    void reset(double value = 0.0) noexcept { y_ = value; }
    double process(double x) noexcept SONORA_RT {
        const double a = x > y_ ? attack_ : release_;
        y_ = a * y_ + (1.0 - a) * x;
        return y_;
    }
    double value() const noexcept { return y_; }

private:
    double attack_ = 0.0, release_ = 0.0, y_ = 0.0;
};

// Mean square with an exponential window, for an RMS detector.
class MeanSquare {
public:
    void set_time(double tau_s, double sample_rate) noexcept {
        a_ = time_constant_coeff(tau_s, sample_rate);
    }
    void reset() noexcept { y_ = 0.0; }
    double process(double x) noexcept SONORA_RT {
        y_ = a_ * y_ + (1.0 - a_) * x * x;
        return y_;
    }

private:
    double a_ = 0.0, y_ = 0.0;
};

// Instant attack, exponential fall: the envelope a gate's open/close
// decision reads, so a low note does not chatter the gate at every zero
// crossing.
class PeakFollower {
public:
    void set_release(double tau_s, double sample_rate) noexcept {
        r_ = time_constant_coeff(tau_s, sample_rate);
    }
    void reset() noexcept { y_ = 0.0; }
    double process(double x) noexcept SONORA_RT {
        const double ax = std::fabs(x);
        y_ = ax > y_ ? ax : r_ * y_;
        return y_;
    }

private:
    double r_ = 0.0, y_ = 0.0;
};

}  // namespace sonora::dsp
