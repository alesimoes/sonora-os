#pragma once

// A reference PatternPlayer (ADR-0169): straight arithmetic over the snapshot,
// for contract tests and as the yardstick the engine's player is checked
// against.

#include "sonora/stepseq/pattern_player.h"

namespace sonora::stepseq {

class FakePatternPlayer final : public PatternPlayer {
public:
    void reset(std::uint64_t seed) noexcept override { seed_ = seed; }
    void render_events(const Pattern& pattern, std::int64_t block_start_frame,
                       std::int32_t frames, const TempoSnapshot& tempo,
                       EventBlock& out) noexcept override;

    // Counter-based, not a running stream: the draw for (pass, step, row) is a
    // pure function of the seed, so seeking and block size change nothing.
    static bool fires(std::uint64_t seed, std::int64_t step_index, int row,
                      int probability) noexcept;

private:
    std::uint64_t seed_ = 0;
};

} // namespace sonora::stepseq
