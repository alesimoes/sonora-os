#pragma once

// A step pattern (ADR-0169): a fixed grid of rows x steps with per-row mute,
// solo, gain and pan, and one swing amount. Fixed capacity, so a copy never
// allocates and the audio thread can read a snapshot of it as plain memory.

#include <array>

#include "sonora/stepseq/row.h"
#include "sonora/stepseq/step.h"

namespace sonora::stepseq {

constexpr int kMaxRows = 16;
constexpr int kMaxSteps = 64;
constexpr int kMinSwingPercent = 50; // straight
constexpr int kMaxSwingPercent = 75; // every second step a full half-step late

class Pattern {
public:
    int length() const noexcept { return length_; }
    int row_count() const noexcept { return row_count_; }
    int steps_per_beat() const noexcept { return steps_per_beat_; }
    int swing_percent() const noexcept { return swing_percent_; }

    // Setters clamp instead of refusing: the editor shows the clamped value.
    void set_length(int steps) noexcept { length_ = clamp(steps, 1, kMaxSteps); }
    void set_row_count(int rows) noexcept { row_count_ = clamp(rows, 1, kMaxRows); }
    // A finer grid shrinks half a step, so stored nudges are clamped again;
    // otherwise a trigger could drift outside the player's scan window.
    void set_steps_per_beat(int n) noexcept {
        steps_per_beat_ = clamp(n, 1, 8);
        const int half = max_nudge_ticks();
        for (auto& row_steps : steps_)
            for (Step& s : row_steps)
                s.nudge_ticks = static_cast<std::int16_t>(clamp(s.nudge_ticks, -half, half));
    }
    void set_swing_percent(int p) noexcept {
        swing_percent_ = clamp(p, kMinSwingPercent, kMaxSwingPercent);
    }

    const Row& row(int r) const noexcept { return rows_[index(r, kMaxRows)]; }
    void set_row(int r, const Row& v) noexcept { rows_[index(r, kMaxRows)] = v; }

    const Step& step(int r, int s) const noexcept {
        return steps_[index(r, kMaxRows)][index(s, kMaxSteps)];
    }
    void set_step(int r, int s, const Step& v) noexcept {
        Step c = clamped(v);
        const int half = max_nudge_ticks();
        c.nudge_ticks = static_cast<std::int16_t>(clamp(c.nudge_ticks, -half, half));
        steps_[index(r, kMaxRows)][index(s, kMaxSteps)] = c;
    }

    // Half a step either way keeps every trigger inside its own step's
    // neighbourhood, so a block scan of one step either side finds it.
    int max_nudge_ticks() const noexcept { return kTicksPerBeat / steps_per_beat_ / 2; }

    bool any_solo() const noexcept {
        for (int r = 0; r < row_count_; ++r)
            if (rows_[r].solo) return true;
        return false;
    }

    bool row_audible(int r, bool any_solo_now) const noexcept {
        const Row& v = row(r);
        if (v.mute || v.source == kNoSource) return false;
        return !any_solo_now || v.solo;
    }

private:
    static int clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
    static int index(int i, int cap) noexcept { return clamp(i, 0, cap - 1); }

    std::array<Row, kMaxRows> rows_{};
    std::array<std::array<Step, kMaxSteps>, kMaxRows> steps_{};
    int length_ = 16;
    int row_count_ = 8;
    int steps_per_beat_ = 4; // sixteenth notes
    int swing_percent_ = kMinSwingPercent;
};

} // namespace sonora::stepseq
