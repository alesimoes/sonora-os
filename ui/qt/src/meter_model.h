// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a meter shows, separated from how it is drawn: the dBFS scale, the
// ballistics (instant attack, timed release, peak hold) and the rectangles
// that result. No Qt here, so every number on screen can be tested without a
// display, and MeterItem is left with copying rectangles into vertices.
#pragma once

#include <cstdint>

namespace sonora::ui {

// Bottom of the scale. Anything quieter, and digital silence, sits at the
// floor; the meter does not pretend to resolve -90 dBFS on 300 pixels.
constexpr float kMeterFloorDb = -60.0f;
// What "no signal at all" is stored as. Below the floor, and finite, so the
// arithmetic of release and hold never meets an infinity.
constexpr float kSilenceDb = -200.0f;

// Linear peak to dBFS. Zero, anything negative and NaN are kSilenceDb;
// +infinity is 0 dBFS, so no infinity reaches the ballistics.
float peak_to_dbfs(float peak);

// The peak that counts as an over. A 24-bit converter's top code is
// 0x7fffff, which arrives as 1 - 2^-23 and never as 1.0, so a test for
// >= 1.0 would miss a clipped 24-bit input. Anything at or above the top
// code is at full scale; 1 - 2^-22, one code down, is not. Until M4's
// clip_count, which the engine counts at the sample, this is the over.
constexpr float kOverPeak = 1.0f - 1.0f / 8388608.0f;

// Where `db` sits on the scale, 0 at the floor and 1 at 0 dBFS. Piecewise
// linear with more room near the top, where a recording engineer reads it:
// the top 20 dB take half the height. Monotonic, and clamped at both ends.
float dbfs_to_fraction(float db);

// The marks drawn beside the bar, in dBFS.
constexpr int kScaleMarkCount = 8;
extern const float kScaleMarksDb[kScaleMarkCount];

// The colour zones of the bar.
constexpr float kAmberFromDb = -18.0f;  // a common line-up level
constexpr float kRedFromDb = -6.0f;

struct MeterBallistics {
    // IEC 60268-18's return time is 20 dB in 1.7 s for a peak programme
    // meter; the bar falls at that rate, and the hold marker does once its
    // hold time has run out.
    float release_db_per_s = 20.0f / 1.7f;
    float hold_s = 2.0f;

    float bar_db = kSilenceDb;
    float hold_db = kSilenceDb;
    float hold_age_s = 0.0f;
    // Latched when a peak reaches full scale, and kept until the operator
    // clears it. A clip indicator that let go on its own would be gone by
    // the time anyone looked, which is the lie this whole project avoids.
    bool over = false;
    // False while the engine is not answering. The meter then draws no bar
    // at all: a frozen value looks exactly like a held note.
    bool has_data = false;

    // One tick: `peak` is the linear maximum since the previous tick, `dt_s`
    // the time since it. Attack is instant, so no peak is ever shown low.
    // A NaN peak carries no level and is ignored; +infinity is past full
    // scale, so it is an over and a bar at 0 dBFS that falls as usual.
    void feed(float peak, float dt_s);
    void set_no_data();
    void clear_over() { over = false; }
};

// One solid rectangle in item coordinates (origin top left), colour as
// 0xAARRGGBB, premultiplied by nothing: every colour here is opaque.
struct MeterRect {
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t argb = 0;
};

// The most rectangles one meter ever emits. MeterItem sizes its geometry for
// this once and never reallocates; unused slots are emitted with zero size.
constexpr int kMeterMaxRects = 6 + kScaleMarkCount;

// Fills `out` with what the meter shows for `m` at size w x h and returns how
// many rectangles it wrote. The over indicator is a band at the top, the bar
// is split into its colour zones, the hold marker is a 2-pixel line, and the
// scale marks sit at the right edge.
int meter_rects(const MeterBallistics& m, float w, float h, MeterRect* out);

// Exposed for tests: the band the over indicator occupies at height h.
float meter_over_band(float h);

}  // namespace sonora::ui
