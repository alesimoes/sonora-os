#pragma once

// Step-to-frame arithmetic shared by the player and its tests (ADR-0169).

#include <cstdint>

#include "sonora/stepseq/pattern.h"
#include "sonora/stepseq/pattern_player.h"

namespace sonora::stepseq {

struct StepTiming {
    double frames_per_step = 0.0;
    double swing_frames = 0.0;
    double frames_per_tick = 0.0;
};

// False when the tempo is unusable; the player then emits nothing.
bool make_step_timing(const Pattern& pattern, const TempoSnapshot& tempo, StepTiming& out) noexcept;

// Timeline frame of absolute step n: odd steps are delayed by swing, then the
// nudge is added. Rounded once, so block cuts cannot move a trigger.
std::int64_t trigger_frame(const StepTiming& t, std::int64_t origin, std::int64_t n,
                           int nudge_ticks) noexcept;

// Inclusive range of absolute steps whose trigger can land in [start, end).
// Swing is at most one step late and nudge half a step early, so two steps of
// margin each side are enough.
void scan_range(const StepTiming& t, std::int64_t origin, std::int64_t start, std::int64_t end,
                std::int64_t& first, std::int64_t& last) noexcept;

} // namespace sonora::stepseq
