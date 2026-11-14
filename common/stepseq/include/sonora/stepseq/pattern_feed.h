#pragma once

// The audio thread's side of the PatternExchange (ADR-0169): one acquire() per
// block, and the snapshot it returns is what the player renders. Taking the
// snapshot once per block keeps a block's events from one consistent pattern
// and is what lets publish() reuse the slot the audio thread let go of.

#include <cstdint>

#include "sonora/stepseq/pattern_exchange.h"
#include "sonora/stepseq/pattern_player.h"

namespace sonora::stepseq {

class PatternFeed {
public:
    PatternFeed(PatternExchange& exchange, PatternPlayer& player) noexcept
        : exchange_(exchange), player_(player) {}

    // Audio thread, once per block. Two atomic stores and loads, then the
    // player: no allocation, lock or wait.
    void render_block(std::int64_t block_start_frame, std::int32_t frames,
                      const TempoSnapshot& tempo, EventBlock& out) noexcept {
        player_.render_events(exchange_.acquire(), block_start_frame, frames, tempo, out);
    }

private:
    PatternExchange& exchange_;
    PatternPlayer& player_;
};

} // namespace sonora::stepseq
