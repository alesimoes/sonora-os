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

// Append-only. A track with an active insert is TrackSource -> InsertChain
// -> TrackFader, the three steps TrackOut fuses (ADR-0012 §7.1).
enum class NodeKind : uint8_t {
    TrackOut = 0,
    BusSum = 1,
    HardwareOut = 2,
    TrackSource = 3,
    InsertChain = 4,
    TrackFader = 5,
};

// An InsertChain node bound to no host channel: the chain passes its input
// through. What every chain is until engine/plugin_client binds one.
constexpr uint32_t kNoChannelSlot = 0xFFFFFFFFu;
// A chain whose parameter store, or fade state, the control thread has not
// assigned. Explicit, never 0: index 0 is a real store and a real state.
constexpr uint32_t kNoParamStore = 0xFFFFFFFFu;
constexpr uint32_t kNoNodeState = 0xFFFFFFFFu;
// An edge that runs through no delay line.
constexpr uint32_t kNoDelayLine = 0xFFFFFFFFu;
constexpr uint32_t kNoNode = 0xFFFFFFFFu;

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

// The graph, in topological order: per track a TrackOut, or TrackSource,
// InsertChain and TrackFader; then one BusSum, one HardwareOut.
struct PlanNode {
    NodeKind kind = NodeKind::TrackOut;
    uint32_t stable_id = 0;          // survives plan swaps
    uint32_t first_input_edge = 0;   // edges are grouped by to_node
    uint32_t input_edge_count = 0;
    uint32_t scratch_buffer = 0;     // stereo, preallocated by the session
    uint32_t track = 0;              // track nodes: index into tracks
    // InsertChain only (ADR-0012 §7.1). Indices, never pointers: the slot
    // and the parameter store live in engine/plugin_client's private
    // memory, and a plan names them as they were when it was built.
    uint32_t channel_slot = kNoChannelSlot;
    uint32_t channel_generation = 0;
    uint32_t param_store = kNoParamStore;   // auditor A2.5
    // The node's miss fade and ramp state, kept for the chain across plans
    // like a delay line (rt-engineer M5 on PR #45).
    uint32_t node_state = kNoNodeState;
    // The chain's checked latency, IPC block included, as read at build.
    // The audio thread never recomputes alignment from it.
    uint32_t latency_frames = 0;
    uint16_t channels = 2;
    // The model track id, for the control thread; the audio thread never
    // reads it.
    uint64_t chain_id = 0;
};

// from_node's output feeds to_node's input, `delay_frames` late: plugin
// delay compensation (ADR-0012 §7.3). The delay line's contents live in
// engine/core, outside the plan, at `delay_line` (kNoDelayLine: none),
// which the builder keeps the same for the same pair of stable ids across
// plans, so a swap never restarts a delay from silence. The session resets
// a line it finds serving a different pair, so a reassigned line never
// plays another edge's past.
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
    // PDC as built (ADR-0012 §7.3): the slowest path's latency at
    // HardwareOut; how far short of the alignment it needs the worst edge
    // was left by the maximum or the delay-line pool (non-zero is red); and
    // how many edges the pool left undelayed.
    uint32_t compensation_frames = 0;
    uint32_t compensation_shortfall_frames = 0;
    uint32_t undelayed_edges = 0;
    // The last InsertChain node, after which the hosts are woken once
    // (ADR-0012 §7.2 step 3); kNoNode when there is none.
    uint32_t last_insert_node = kNoNode;
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
