// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fade law, in one place: engine/core plays it, ui/qt draws it. It lives
// in common/base because that is the one module both may include (ADR-0007
// §1), and because the audio thread calls it, which only common/base and
// engine/core may contribute (ADR-0007 §2). A copy on each side could drift
// with both test suites still green; one definition cannot.
#pragma once

#include <cmath>
#include <cstdint>

#include "sonora/rt.h"

namespace sonora {

// The values match model::FadeShape, so engine/control and ui/qt cast.
enum class FadeCurve : uint8_t { Linear = 0, EqualPower = 1, SCurve = 2 };

// The gain at position x in [0, 1] of a fade in of this shape. A fade out
// is fade_in_gain(shape, 1 - x), so a crossfade's two sides sum to 1
// (linear, s-curve) or their squares do (equal power).
inline float fade_in_gain(FadeCurve shape, float x) noexcept SONORA_RT {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    constexpr float kHalfPi = 1.57079632679489662f;
    switch (shape) {
        case FadeCurve::EqualPower:
            return std::sin(kHalfPi * x);
        case FadeCurve::SCurve:
            return 0.5f - 0.5f * std::cos(2.0f * kHalfPi * x);
        case FadeCurve::Linear:
            break;
    }
    return x;
}

}  // namespace sonora
