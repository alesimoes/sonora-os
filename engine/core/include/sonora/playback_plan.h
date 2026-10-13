// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playback plan: what the audio thread reads to play the timeline
// (brain/specs/timeline-model.md, "The playback plan"; ADR-0009 §4).
//
// engine/control builds it from a model snapshot; engine/core defines its
// layout and executes it. One allocation, every byte written by
// create_plan() before the builder fills it, so no page of it is first
// touched by the audio thread. No pointer into the model, nothing to
// destroy. The structure is immutable once published; the chunk-slot array
// is its one mutable part, written only by the disk reader.
#pragma once

#include <atomic>
#include <cstdint>

#include "sonora/rt.h"

namespace sonora {

// A chunk slot that names no pool buffer: the audio thread plays silence
// there and counts a miss.
constexpr uint32_t kNoChunk = 0xFFFFFFFFu;

// The values match model::FadeShape, so the builder casts.
enum class FadeCurve : uint8_t { Linear = 0, EqualPower = 1, SCurve = 2 };

enum class NodeKind : uint8_t { TrackOut = 0, BusSum = 1, HardwareOut = 2 };

// A region, flattened. A crossfade is two segments that overlap by its
// length, each with its fade set over the overlap: the only place two
// segments of a track overlap. Fades run over [start, fade_in_end) and
// [fade_out_start, end).
struct PlanSegment {
    int64_t start = 0;
    int64_t end = 0;
    int64_t source_offset = 0;   // source frame that plays at `start`
    int64_t fade_in_end = 0;
    int64_t fade_out_start = 0;
    uint32_t source_slot[2] = {0, 0};        // DiskReader source table index
    uint16_t source_channel[2] = {0, 0};
    // Channel c plays source chunk k from chunk_slots[first_chunk_slot[c] +
    // k - first_chunk_index[c]], for chunk_count[c] chunks.
    uint32_t first_chunk_slot[2] = {0, 0};
    uint32_t first_chunk_index[2] = {0, 0};
    uint32_t chunk_count[2] = {0, 0};
    uint16_t channels = 1;
    float gain = 1.0f;                       // linear region gain
    FadeCurve fade_in_shape = FadeCurve::Linear;
    FadeCurve fade_out_shape = FadeCurve::Linear;
};

struct PlanTrack {
    uint32_t engine_track = 0;   // index into Session::tracks()
    uint32_t first_segment = 0;  // sorted by start, and by end
    uint32_t segment_count = 0;
    uint16_t channels = 1;
};

// The graph, in topological order. Phase 2's is fixed: a TrackOut per
// track, one BusSum, one HardwareOut.
struct PlanNode {
    NodeKind kind = NodeKind::TrackOut;
    uint32_t stable_id = 0;          // survives plan swaps
    uint32_t first_input_edge = 0;   // edges are grouped by to_node
    uint32_t input_edge_count = 0;
    uint32_t scratch_buffer = 0;     // stereo, preallocated by the session
    uint32_t track = 0;              // TrackOut: index into tracks
};

// from_node's output feeds to_node's input, `delay_frames` late. The field
// is where plugin delay compensation goes (Phase 4); 0 in Phase 2. The
// delay line's contents live in engine/core, outside the plan, at
// `delay_line`, which the builder keeps the same for the same edge across
// plans, so a swap never restarts a delay from silence.
struct PlanEdge {
    uint32_t from_node = 0;
    uint32_t to_node = 0;
    uint32_t delay_frames = 0;
    uint32_t delay_line = 0;
};

struct PlanSizes {
    uint32_t tracks = 0;
    uint32_t segments = 0;
    uint32_t nodes = 0;
    uint32_t edges = 0;
    uint32_t chunk_slots = 0;
};

struct PlaybackPlan {
    // How the audio thread detects a new plan. Never the pointer: a freed
    // plan's address can come back for the next one (ADR-0009 §4.1).
    uint64_t epoch = 0;
    uint64_t model_revision = 0;
    PlanTrack* tracks = nullptr;
    uint32_t track_count = 0;
    PlanSegment* segments = nullptr;
    uint32_t segment_count = 0;
    PlanNode* nodes = nullptr;
    uint32_t node_count = 0;
    PlanEdge* edges = nullptr;
    uint32_t edge_count = 0;
    std::atomic<uint32_t>* chunk_slots = nullptr;
    uint32_t chunk_slot_count = 0;
    uint32_t chunk_frames = 0;           // the chunk size the slots assume
    uint64_t peak_chunk_demand = 0;      // max over window positions
    uint32_t monitor_out[2] = {0, 1};    // indices into AudioBlock::outputs
    // Mono tracks pan as ((1 -+ pan) / 2) ^ pan_exponent, which is
    // pan_law_db at centre; precomputed so the audio thread takes no log.
    float pan_exponent = 0.5f;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "chunk slots");

// One allocation of exactly these sizes, every byte written, every chunk
// slot kNoChunk, counts set and everything else zero. Never on the audio
// thread. Returns nullptr only if the allocation fails.
PlaybackPlan* create_plan(const PlanSizes& sizes);
void destroy_plan(PlaybackPlan* plan);

// Phase 2's fixed graph over the plan's tracks: TrackOut i for track i, then
// one BusSum, then one HardwareOut; edges grouped by to_node, delays 0,
// delay line = edge index. Needs nodes == tracks + 2 and edges == tracks + 1.
// Stable ids: track i's TrackOut is i + 1, so a swap that keeps the tracks
// keeps the ids.
void layout_monitor_graph(PlaybackPlan& plan);

constexpr uint32_t kBusSumStableId = 0xFFFF0001u;
constexpr uint32_t kHardwareOutStableId = 0xFFFF0002u;

// The slot that holds source chunk `chunk` of segment channel `ch`, or
// kNoChunk if the segment does not reach that chunk.
inline uint32_t chunk_slot_of(const PlanSegment& s, uint32_t ch,
                              uint64_t chunk) noexcept SONORA_RT {
    if (chunk < s.first_chunk_index[ch]) return kNoChunk;
    const uint64_t k = chunk - s.first_chunk_index[ch];
    if (k >= s.chunk_count[ch]) return kNoChunk;
    return s.first_chunk_slot[ch] + static_cast<uint32_t>(k);
}

// The gain at position x in [0, 1] of a fade in of this shape. A fade out
// is fade_in_gain(shape, 1 - x), so a crossfade's two sides sum to 1
// (linear, s-curve) or their squares do (equal power).
float fade_in_gain(FadeCurve shape, float x) noexcept SONORA_RT;

}  // namespace sonora
