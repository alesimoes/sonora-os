// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fade law as a crossfade pairs it (engine.timeline.crossfades). Frame
// i of an L-frame crossfade fades in at x = i / L and out at 1 - x, as
// Session's segment_gain computes them, in float. An equal-gain pair
// (linear, s-curve) must sum to unity, so correlated material (a region
// split and crossfaded against itself) passes through unchanged; an
// equal-power pair must sum to unity in power, so uncorrelated material
// keeps its level through the cut.
#include "sonora/fade_curve.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::FadeCurve;
using sonora::fade_in_gain;

constexpr FadeCurve kShapes[] = {FadeCurve::Linear, FadeCurve::EqualPower, FadeCurve::SCurve};
// Odd and even lengths, one frame, and a second at 48 kHz.
constexpr int64_t kLengths[] = {1, 2, 3, 64, 127, 1000, 48000};

bool equal_gain(FadeCurve s) { return s != FadeCurve::EqualPower; }

int test_ends_and_monotony() {
    for (FadeCurve s : kShapes) {
        CHECK(fade_in_gain(s, 0.0f) == 0.0f);
        CHECK(fade_in_gain(s, 1.0f) == 1.0f);
        // Out of range clamps, so a frame just outside a fade is silent or
        // unity, never a negative or overshooting gain.
        CHECK(fade_in_gain(s, -0.5f) == 0.0f);
        CHECK(fade_in_gain(s, 1.5f) == 1.0f);
        float last = 0.0f;
        for (int i = 0; i <= 4096; ++i) {
            const float g = fade_in_gain(s, static_cast<float>(i) / 4096.0f);
            CHECK(g >= last && g >= 0.0f && g <= 1.0f);
            last = g;
        }
    }
    return 0;
}

int test_pairs() {
    for (FadeCurve s : kShapes) {
        for (int64_t len : kLengths) {
            double worst = 0.0;
            for (int64_t i = 0; i < len; ++i) {
                const float x = static_cast<float>(i) / static_cast<float>(len);
                const float in = fade_in_gain(s, x);
                const float out = fade_in_gain(s, 1.0f - x);
                const double sum = equal_gain(s) ? double(in) + out : double(in) * in + double(out) * out;
                const double err = std::fabs(sum - 1.0);
                if (err > worst) worst = err;
            }
            if (worst > 2e-6) {
                std::fprintf(stderr, "  shape %d length %lld: off unity by %g\n",
                             static_cast<int>(s), static_cast<long long>(len), worst);
                return 1;
            }
        }
    }
    return 0;
}

// Why equal gain is the shape for correlated material: at the middle of an
// equal-power crossfade of a signal against itself the sum is +3 dB; an
// equal-gain one is flat there, and -6 dB per side where equal power is -3.
int test_middle() {
    const float half = 0.5f;
    const float ep = fade_in_gain(FadeCurve::EqualPower, half);
    CHECK(std::fabs(ep - std::sqrt(0.5f)) < 1e-6f);
    CHECK(std::fabs(2.0f * ep - std::sqrt(2.0f)) < 1e-6f);
    CHECK(fade_in_gain(FadeCurve::Linear, half) == 0.5f);
    CHECK(std::fabs(fade_in_gain(FadeCurve::SCurve, half) - 0.5f) < 1e-6f);
    return 0;
}

}  // namespace

int main() {
    if (test_ends_and_monotony()) return 1;
    if (test_pairs()) return 1;
    if (test_middle()) return 1;
    std::printf("fade_curve: ok\n");
    return 0;
}
