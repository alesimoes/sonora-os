// SPDX-License-Identifier: GPL-3.0-or-later
//
// Branch-free float exp, tanh and sigmoid for the neural kernels.
//
// Why not std::tanh: the "standard" NAM WaveNet evaluates 240 activations
// per sample, about 31 000 per 128-frame block, and a libm call per value
// does not vectorise. These are plain arithmetic, a select and a bit cast,
// which the compilers vectorise in the activation loops, and they give the
// same bits on every platform (no libm in the result, so the equivalence
// test does not depend on which libm the build has).
//
// Accuracy, held by common/dsp/tests/test_neural.cpp against std::tanh and
// the sigmoid computed in double over [-20, 20]: absolute error <= 2.5e-7.
// The equivalence bar is 1e-5 on the output (ADR-0011 §3.2).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sonora::dsp::neural::detail {

// e^y for y in [-80, 0]. Cody and Waite's range reduction (W. J. Cody and
// W. Waite, "Software Manual for the Elementary Functions", 1980): y =
// n ln2 + r with |r| <= ln2/2, ln2 split in a high part with few enough
// bits that n * high is exact for |n| < 2^15, then e^r by its Taylor series
// to r^7 (truncation r^8/8! < 5e-9 at |r| = 0.347) and 2^n built in the
// exponent field. n >= -116 keeps 2^n a normal float.
inline float exp_nonpositive(float y) noexcept {
    const float t = y * 1.44269504088896341f;
    // t <= 0: truncating t - 0.5 rounds to nearest (cvttps2dq on SSE2).
    const auto n = static_cast<std::int32_t>(t - 0.5f);
    const auto fn = static_cast<float>(n);
    const float r = (y - fn * 0.693359375f) - fn * -2.12194440054690583e-4f;
    float p = 1.0f / 5040.0f;
    p = p * r + 1.0f / 720.0f;
    p = p * r + 1.0f / 120.0f;
    p = p * r + 1.0f / 24.0f;
    p = p * r + 1.0f / 6.0f;
    p = p * r + 0.5f;
    p = p * r + 1.0f;
    p = p * r + 1.0f;
    const std::int32_t bits = (n + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

// c ? a : b through the bits. Written as a ternary, GCC sinks each arm's
// arithmetic into a branch and then, under the default -ftrapping-math,
// will not speculate it back: the activation loop stayed scalar (320 us per
// 30 720 tanh in the build image against 145 us with this). Exact: the
// chosen value's bits are passed through.
inline float select_bits(bool c, float a, float b) noexcept {
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof ua);
    std::memcpy(&ub, &b, sizeof ub);
    const std::uint32_t mask = 0u - static_cast<std::uint32_t>(c);
    const std::uint32_t r = (ua & mask) | (ub & ~mask);
    float f;
    std::memcpy(&f, &r, sizeof f);
    return f;
}

// tanh. Below 0.25 the odd Taylor series to x^9 (next term < 2.2e-9), so
// small values keep their relative accuracy; above, (1 - e)/(1 + e) with
// e = e^(-2|x|), which has no cancellation there. |x| is held to 9, where
// tanh rounds to 1 in float.
inline float tanh_approx(float x) noexcept {
    const float ax = std::fabs(x);
    const float a = ax < 9.0f ? ax : 9.0f;
    const float e = exp_nonpositive(-2.0f * a);
    const float big = (1.0f - e) / (1.0f + e);
    const float a2 = a * a;
    float s = 62.0f / 2835.0f;
    s = s * a2 - 17.0f / 315.0f;
    s = s * a2 + 2.0f / 15.0f;
    s = s * a2 - 1.0f / 3.0f;
    s = s * a2 + 1.0f;
    const float small = a * s;
    return std::copysign(select_bits(a < 0.25f, small, big), x);
}

// 1 / (1 + e^-x), with e = e^-|x| so neither branch overflows; |x| held
// to 80, where the result is within 2e-35 of 0 or 1.
inline float sigmoid_approx(float x) noexcept {
    const float ax = std::fabs(x);
    const float e = exp_nonpositive(-(ax < 80.0f ? ax : 80.0f));
    const float s = 1.0f / (1.0f + e);
    return select_bits(x >= 0.0f, s, e * s);
}

// The format's "Fasttanh": a rational approximation the NAM project
// defines (NeuralAmpModelerCore, NAM/activations.h, fast_tanh, MIT). A
// file that names it was trained against exactly this function, so it is
// reproduced coefficient for coefficient.
inline float fast_tanh(float x) noexcept {
    const float ax = std::fabs(x);
    const float x2 = x * x;
    return (x * (2.45550750702956f + 2.45550750702956f * ax +
                 (0.893229853513558f + 0.821226666969744f * ax) * x2) /
            (2.44506634652299f + (2.44506634652299f + x2) * std::fabs(x + 0.814642734961073f * x * ax)));
}

}  // namespace sonora::dsp::neural::detail
