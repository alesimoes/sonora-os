// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0149: how far a nudge moves, apart from the grid. UI only, never
// saved: it resets when the application starts and when a session opens.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "time_format.h"

namespace sonora::ui {

enum class NudgeValue { FollowGrid, Bar, Beat, Eighth, Sixteenth, Ms100, Ms10, Ms1 };

inline NudgeValue next_nudge_value(NudgeValue v) {
    return v == NudgeValue::Ms1 ? NudgeValue::FollowGrid
                                : static_cast<NudgeValue>(static_cast<int>(v) + 1);
}

inline const char* nudge_value_name(NudgeValue v) {
    switch (v) {
        case NudgeValue::FollowGrid: return "GRID";
        case NudgeValue::Bar: return "BAR";
        case NudgeValue::Beat: return "1/4";
        case NudgeValue::Eighth: return "1/8";
        case NudgeValue::Sixteenth: return "1/16";
        case NudgeValue::Ms100: return "100 ms";
        case NudgeValue::Ms10: return "10 ms";
        case NudgeValue::Ms1: return "1 ms";
    }
    return "GRID";
}

// A millisecond value is absolute time, so the tempo map plays no part; one
// frame at least, so a nudge never silently does nothing.
inline model::Frames nudge_ms_frames(double ms, std::uint32_t rate) {
    return std::max<model::Frames>(1, static_cast<model::Frames>(std::llround(rate * ms / 1000.0)));
}

// Where one nudge from `f` lands, `direction` 1 or -1; never below 0.
inline model::Frames nudge_value_target(model::Frames f, int direction, NudgeValue v,
                                        GridValue grid, const model::SessionSettings& s) {
    GridValue g = grid;
    double ms = 0;
    switch (v) {
        case NudgeValue::FollowGrid: break;
        case NudgeValue::Bar: g = GridValue::Bar; break;
        case NudgeValue::Beat: g = GridValue::Beat; break;
        case NudgeValue::Eighth: g = GridValue::Eighth; break;
        case NudgeValue::Sixteenth: g = GridValue::Sixteenth; break;
        case NudgeValue::Ms100: ms = 100; break;
        case NudgeValue::Ms10: ms = 10; break;
        case NudgeValue::Ms1: ms = 1; break;
    }
    if (ms == 0) return nudge_target(f, direction, g, s);
    const model::Frames step = nudge_ms_frames(ms, s.sample_rate);
    return std::max<model::Frames>(0, direction < 0 ? f - step : f + step);
}

}  // namespace sonora::ui
