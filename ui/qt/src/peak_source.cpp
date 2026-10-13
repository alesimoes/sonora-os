// SPDX-License-Identifier: GPL-3.0-or-later
#include "peak_source.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;

// A stable pseudo-random value in [0, 1) for (source, step): the same bucket
// gets the same jitter however often the timeline is redrawn or scrolled.
float hash01(uint64_t a, uint64_t b) {
    uint64_t x = a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull);
    x ^= x >> 31;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    return static_cast<float>(x >> 40) / static_cast<float>(1ull << 24);
}

}  // namespace

float SyntheticPeakSource::envelope(model::Id source, model::Frames frame) const {
    if (frame < 0 || rate_ == 0) return 0.0f;
    const double t = static_cast<double>(frame) / rate_;
    const double beat = 60.0 / bpm_;
    const uint64_t step = static_cast<uint64_t>(frame / std::max<uint32_t>(1, rate_ / 200));
    const float grain = 0.85f + 0.15f * hash01(source, step);
    switch (source % 3) {
        case 0: {  // percussive: a hit on every beat, a smaller one off it
            const double ph = std::fmod(t, beat) / beat;
            const double off = std::fmod(t + beat / 2, beat) / beat;
            const double v = 0.9 * std::exp(-ph * 14.0) + 0.35 * std::exp(-off * 18.0);
            return static_cast<float>(std::min(1.0, v + 0.03)) * grain;
        }
        case 1: {  // sustained: a slow swell, as a bass or a pad
            const double v = 0.45 + 0.2 * std::sin(2 * kPi * t / (beat * 8) + source) +
                             0.12 * std::sin(2 * kPi * t / beat);
            return static_cast<float>(std::clamp(v, 0.05, 0.95)) * grain;
        }
        default: {  // phrased: sung lines with breaths between them
            const double bar = beat * 4;
            const double ph = std::fmod(t, bar * 2) / (bar * 2);
            const double gate = ph < 0.8 ? std::sin(kPi * ph / 0.8) : 0.02;
            const double v = gate * (0.55 + 0.3 * std::sin(2 * kPi * t * 0.7 + source));
            return static_cast<float>(std::clamp(v, 0.01, 0.95)) * grain;
        }
    }
}

void SyntheticPeakSource::peaks(model::Id source, uint16_t channel, model::Frames first,
                                double frames_per_bucket, int count, PeakPair* out) const {
    (void)channel;
    const double fpb = std::max(1.0, frames_per_bucket);
    for (int i = 0; i < count; ++i) {
        const double a = static_cast<double>(first) + i * fpb;
        // Four probes a bucket: enough for the shape of an overview, and the
        // cost of a full-window rebuild stays a few thousand evaluations.
        float e = 0.0f;
        for (int k = 0; k < 4; ++k) {
            e = std::max(e, envelope(source, static_cast<model::Frames>(a + fpb * k / 4.0)));
        }
        const float asym = 0.9f + 0.1f * hash01(source + 7, static_cast<uint64_t>(a));
        out[i] = PeakPair{-e * asym, e};
    }
}

}  // namespace sonora::ui
