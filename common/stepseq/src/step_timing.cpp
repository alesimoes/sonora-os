#include "step_timing.h"

#include <cmath>

namespace sonora::stepseq {

bool make_step_timing(const Pattern& pattern, const TempoSnapshot& tempo, StepTiming& out) noexcept {
    if (!(tempo.sample_rate > 0.0) || !(tempo.bpm > 0.0)) return false;
    const double per_beat = tempo.sample_rate * 60.0 / tempo.bpm;
    out.frames_per_step = per_beat / pattern.steps_per_beat();
    out.swing_frames = (pattern.swing_percent() / 100.0 - 0.5) * 2.0 * out.frames_per_step;
    out.frames_per_tick = per_beat / kTicksPerBeat;
    return true;
}

std::int64_t trigger_frame(const StepTiming& t, std::int64_t origin, std::int64_t n,
                           int nudge_ticks) noexcept {
    const double base = static_cast<double>(n) * t.frames_per_step + ((n % 2) ? t.swing_frames : 0.0);
    return origin + std::llround(base + nudge_ticks * t.frames_per_tick);
}

void scan_range(const StepTiming& t, std::int64_t origin, std::int64_t start, std::int64_t end,
                std::int64_t& first, std::int64_t& last) noexcept {
    first = static_cast<std::int64_t>(std::floor((start - origin) / t.frames_per_step)) - 2;
    last = static_cast<std::int64_t>(std::floor((end - origin) / t.frames_per_step)) + 2;
    if (first < 0) first = 0;
}

} // namespace sonora::stepseq
