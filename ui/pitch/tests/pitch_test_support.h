// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the ui/pitch tests share: a CHECK that names the line, notes made by
// hand (as segment_notes would leave them) and a region to hold them.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/ui_pitch/pitch_editor_model.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace pitchtest {

constexpr uint32_t kRate = 48000;
constexpr std::size_t kHop = 240;  // 5 ms

// A flat note: `cents` from A4 from frame `start` to `end`, contour included.
inline sonora::pitch::Note flat_note(std::size_t start, std::size_t end, double cents) {
    sonora::pitch::Note n;
    n.start = start;
    n.end = end;
    n.median_cents = cents;
    n.hop = kHop;
    n.rate = kRate;
    n.contour_cents.assign((end - start) / kHop, float(cents));
    return n;
}

// Three notes over a 3 s region at timeline frame 48000: A3, C4 then E4,
// half a second each with gaps between.
inline std::vector<sonora::pitch::Note> three_notes() {
    return {flat_note(24000, 48000, -1200.0), flat_note(48000, 72000, -900.0),
            flat_note(96000, 120000, -500.0)};
}

inline sonora::ui_pitch::PitchRegion region() {
    sonora::ui_pitch::PitchRegion r;
    r.id = 7;
    r.position = 48000;
    r.length = 144000;
    r.rate = kRate;
    r.name = "Lead 1";
    return r;
}

// A sine whose pitch in cents from A4 is cents_at(seconds).
template <typename F>
std::vector<float> tone(double seconds, uint32_t rate, F cents_at) {
    std::vector<float> out(std::size_t(seconds * rate));
    double phase = 0.0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double hz = 440.0 * std::exp2(cents_at(double(i) / rate) / 1200.0);
        out[i] = float(0.5 * std::sin(phase));
        phase += 2.0 * 3.14159265358979323846 * hz / rate;
    }
    return out;
}

}  // namespace pitchtest
