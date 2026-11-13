#pragma once

// Launch quantisation (ADR-0164): where a queued clip start or stop lands.
// Pure functions, no allocation; safe on the audio thread.

#include <cstdint>

namespace sonora::launch {

enum class Quantize : std::uint8_t { Immediate, Beat, Bar, Bars2, Bars4 };

// The tempo and meter the launcher quantises against, read once per block.
// Constant tempo inside one snapshot; a tempo map is the caller's to slice.
// anchor_frame is the frame of bar 1, beat 1: every boundary is counted from
// it, so two snapshots with the same anchor agree on every bar line.
struct TransportSnapshot {
    double sample_rate = 48000.0;
    double tempo_bpm = 120.0;
    std::uint32_t beats_per_bar = 4;
    std::int64_t anchor_frame = 0;
};

// Length of one quantisation step in frames (fractional: 44.1 kHz at 123 BPM
// has no whole-frame beat). 0 for Immediate or an invalid snapshot.
double step_frames(const TransportSnapshot& t, Quantize q);

// Frame of boundary k: anchor + round(k * step). Each boundary is computed
// from k alone, never by accumulating steps, so it does not drift.
std::int64_t boundary_frame(const TransportSnapshot& t, Quantize q, std::int64_t k);

// The first boundary at or after pos. A pos on a boundary is that boundary.
// Immediate, or a snapshot with no tempo, returns pos.
std::int64_t next_boundary(std::int64_t pos_frames, const TransportSnapshot& t, Quantize q);

}  // namespace sonora::launch
