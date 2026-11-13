#pragma once

// Feeds a Launcher from the audio callback (ADR-0164, Consequences): one
// process() per block with the transport as the callback sees it.
//
// The launcher runs on its own launch clock, a frame counter that advances
// by every block whether the transport rolls or not and never jumps. The
// transport only moves the bar grid: while rolling, the grid's anchor is
// placed so that the transport's bar 1 falls on it; while stopped, the grid
// keeps the last anchor and free-runs, so launches still land on bar lines
// a performer can count. A locate, a loop wrap or a tempo change moves the
// grid, never the clock, and the launcher re-snaps its pending actions.
//
// A loop wrap inside the block splits it at the wrap frame into segments,
// each advanced with its own anchor; their transitions are merged with
// offsets counted from the block's first frame.

#include "sonora/launch/launcher.h"

#include <cstdint>
#include <vector>

namespace sonora::launch {

// The transport at the first frame of the block. Positions are transport
// frames (the arrangement timeline), not launch-clock frames.
struct BlockTransport {
    bool rolling = false;
    std::int64_t position = 0;
    double sample_rate = 48000.0;
    double tempo_bpm = 120.0;
    std::uint32_t beats_per_bar = 4;
    // Transport frame of bar 1, beat 1.
    std::int64_t bar1_position = 0;
    bool looping = false;
    std::int64_t loop_start = 0;
    std::int64_t loop_end = 0;
};

class LaunchClock {
public:
    // Sizes the merged list from the launcher's grid; nothing allocates later.
    explicit LaunchClock(Launcher& launcher);

    // Audio thread: no allocation, lock or throw. The list stays valid until
    // the next call.
    TransitionList process(const BlockTransport& transport, std::uint32_t frames);

    // Launch-clock frame of the next block's first frame.
    std::int64_t now() const { return clock_; }
    // Launch-clock frame of bar 1 on the current grid.
    std::int64_t anchor() const { return anchor_; }

private:
    // Frames of the block left before the transport wraps, or `remaining`.
    static std::uint32_t segment_length(const BlockTransport& t, std::int64_t position,
                                        std::uint32_t remaining);
    void run_segment(const BlockTransport& t, std::int64_t position, std::uint32_t frames,
                     std::uint32_t block_offset);

    Launcher& launcher_;
    std::vector<Transition> merged_;
    std::uint32_t count_ = 0;
    std::uint32_t dropped_ = 0;
    std::int64_t clock_ = 0;
    std::int64_t anchor_ = 0;
};

}  // namespace sonora::launch
