#pragma once

// The engine's PatternPlayer (ADR-0169): sample-exact steps with swing, nudge,
// gate, seeded probability, mute/solo and loop wrap. Fixed storage only.

#include "sonora/stepseq/pattern_player.h"

namespace sonora::stepseq {

class StepPatternPlayer final : public PatternPlayer {
public:
    void reset(std::uint64_t seed) noexcept override { seed_ = seed; }
    void render_events(const Pattern& pattern, std::int64_t block_start_frame,
                       std::int32_t frames, const TempoSnapshot& tempo,
                       EventBlock& out) noexcept override;

private:
    std::uint64_t seed_ = 0;
};

} // namespace sonora::stepseq
