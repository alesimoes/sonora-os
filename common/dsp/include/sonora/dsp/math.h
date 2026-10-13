// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scalar helpers shared by every common/dsp block. Everything here is
// bounded arithmetic: safe on the audio thread (ADR-0011 §2.3).
#pragma once

#include <cmath>
#include <cstdint>

namespace sonora::dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

template <class T>
constexpr T clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// exp rather than pow(10, x): the same value to a few ulps, at a fraction
// of the cost, and the dynamics processors call it per sample.
inline double db_to_gain(double db) noexcept { return std::exp(db * 0.11512925464970228); }

// The floor keeps log(0) out of the gain computers: a silent input is
// "very quiet", never -inf, so no NaN reaches a smoother.
inline double gain_to_db(double gain, double floor_db = -200.0) noexcept {
    if (!(gain > 0.0)) return floor_db;
    const double db = 20.0 * std::log10(gain);
    return db < floor_db ? floor_db : db;
}

// One-pole coefficient for a time constant: the step response reaches
// 1 - 1/e (63 %) after tau seconds. This is the definition every attack,
// release and smoothing time in the suite uses (Giannoulis, Massberg and
// Reiss, JAES 60(6), 2012, §3), so the measured 63 % time equals the
// displayed value.
inline double time_constant_coeff(double tau_seconds, double sample_rate) noexcept {
    if (!(tau_seconds > 0.0) || !(sample_rate > 0.0)) return 0.0;
    return std::exp(-1.0 / (tau_seconds * sample_rate));
}

inline bool is_finite(float x) noexcept { return std::isfinite(x); }
inline bool is_finite(double x) noexcept { return std::isfinite(x); }

}  // namespace sonora::dsp
