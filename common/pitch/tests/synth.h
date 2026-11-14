// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test signals for common/pitch. Deterministic: no random_device, a fixed LCG.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace pitchtest {

constexpr double kPi = 3.14159265358979323846;

// A tone whose pitch in cents from A4 at time t (seconds) is cents_at(t),
// amplitude amp, phase kept continuous. Silent where `on` is false.
inline std::vector<float> tone(double seconds, uint32_t rate,
                               const std::function<double(double)>& cents_at, double amp = 0.5,
                               const std::function<bool(double)>& on = {}) {
    std::vector<float> x(std::size_t(seconds * rate));
    double phase = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = double(i) / rate;
        phase += 2.0 * kPi * 440.0 * std::exp2(cents_at(t) / 1200.0) / rate;
        x[i] = (!on || on(t)) ? float(amp * std::sin(phase)) : 0.0f;
    }
    return x;
}

inline std::vector<float> noise(double seconds, uint32_t rate, double amp = 0.3) {
    std::vector<float> x(std::size_t(seconds * rate));
    uint32_t s = 12345;
    for (float& v : x) {
        s = s * 1664525u + 1013904223u;
        v = float(amp * (double(s >> 8) / double(1u << 24) * 2.0 - 1.0));
    }
    return x;
}

}  // namespace pitchtest
