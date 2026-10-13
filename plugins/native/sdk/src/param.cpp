// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/param.h"

#include <cmath>

namespace sonora::native {

namespace {
const char* const kOffOn[] = {"off", "on"};
}

const ParamInfo kBypassParam = {
    "bypass", "Bypass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f,
    // The crossfade is the base class's own (10 ms), not a parameter ramp.
    0.0f, true, false, false, "%.0f", kOffOn, 2,
};

float sanitize(const ParamInfo& p, float value) noexcept {
    if (!std::isfinite(value)) return p.def;
    if (value < p.min) value = p.min;
    if (value > p.max) value = p.max;
    if (p.taper == Taper::Stepped) value = std::round(value);
    return value;
}

double to_normalized(const ParamInfo& p, double v) noexcept {
    if (p.max <= p.min) return 0.0;
    if (p.taper == Taper::Log) {
        return std::log(v / p.min) / std::log(static_cast<double>(p.max) / p.min);
    }
    return (v - p.min) / (static_cast<double>(p.max) - p.min);
}

double from_normalized(const ParamInfo& p, double n) noexcept {
    if (n < 0.0) n = 0.0;
    if (n > 1.0) n = 1.0;
    double v;
    if (p.taper == Taper::Log) {
        v = p.min * std::pow(static_cast<double>(p.max) / p.min, n);
    } else {
        v = p.min + n * (static_cast<double>(p.max) - p.min);
    }
    if (p.taper == Taper::Stepped) v = std::round(v);
    return v;
}

}  // namespace sonora::native
