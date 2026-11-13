#pragma once

// Turns a pattern snapshot into sample-exact trigger events, one audio block at
// a time (ADR-0169). Called from the audio thread: implementations must not
// allocate, lock, block, log or throw.

#include <array>
#include <cstdint>

#include "sonora/stepseq/pattern.h"

namespace sonora::stepseq {

// What the player needs of the transport for one block, copied by the caller
// before the call so the player reads no shared state. Tempo is constant over
// a block; a tempo change lands on the next block boundary.
struct TempoSnapshot {
    double sample_rate = 48000.0;
    double bpm = 120.0;
    // Timeline frame where step 0 of the first pass of the pattern falls.
    std::int64_t origin_frame = 0;
};

struct TriggerEvent {
    std::int32_t frame_offset = 0; // within the block, 0..frames-1
    std::int32_t gate_frames = 0;
    std::int64_t step_index = 0;   // absolute: pass * length + step
    SourceId source = kNoSource;
    float gain_db = 0.0f;
    float pan = 0.0f;
    std::uint8_t row = 0;
    std::uint8_t velocity = 0;
    std::int8_t pitch_semitones = 0;
};

constexpr int kMaxEventsPerBlock = 256;

struct EventBlock {
    std::array<TriggerEvent, kMaxEventsPerBlock> events{};
    int count = 0;
    // Triggers that did not fit; counted, never waited for (an overrun).
    int dropped = 0;

    void clear() noexcept { count = 0; dropped = 0; }
};

class PatternPlayer {
public:
    virtual ~PatternPlayer() = default;

    // Off the audio thread (session open, transport locate): picks the seed of
    // the probability draws, so the same seed replays the same performance.
    virtual void reset(std::uint64_t seed) noexcept = 0;

    // Clears `out` and fills it with the triggers whose frame falls in
    // [block_start_frame, block_start_frame + frames), sorted by frame. The
    // result does not depend on how the timeline is cut into blocks.
    virtual void render_events(const Pattern& pattern, std::int64_t block_start_frame,
                               std::int32_t frames, const TempoSnapshot& tempo,
                               EventBlock& out) noexcept = 0;
};

} // namespace sonora::stepseq
