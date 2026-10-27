// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playback plan's graph with insert chains, and plugin delay
// compensation over it (ADR-0012 §7.1, §7.3; plugin-latency-compensation.md).
//
// In engine/core beside layout_monitor_graph() because it is arithmetic on
// the plan's own layout, which engine/core defines; engine/control's builder
// calls it, and so do engine/core's tests, which need the same alignment the
// builder produces without a model. Never on the audio thread: it allocates.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/playback_plan.h"

namespace sonora {

// One track as the control thread knows it when it builds a plan: the key
// its nodes' stable ids derive from, and its chain. Inactive: the track is
// a TrackOut, with no latency.
struct ChainBinding {
    // Never a track's position: a delete or a reorder would hand one track
    // another's delay history. engine/control gives each model track a key
    // for the session (PlanContinuity); 0 falls back to position + 1, for
    // plans built by hand.
    uint32_t stable_key = 0;
    bool active = false;
    uint64_t chain_id = 0;   // the model track id
    uint32_t channel_slot = kNoChannelSlot;
    uint32_t channel_generation = 0;
    uint32_t param_store = kNoParamStore;
    uint32_t node_state = kNoNodeState;
    uint32_t latency_frames = 0;
};

// The plan sizes for `tracks` tracks of which `chains` carry an active
// chain: three nodes and two inner edges more per chain.
inline uint32_t graph_nodes(uint32_t tracks, uint32_t chains) { return tracks + 2 * chains + 2; }
inline uint32_t graph_edges(uint32_t tracks, uint32_t chains) { return tracks + 2 * chains + 1; }

// Stable ids: a track's TrackOut is its key, its chain nodes the key with a
// tag. Keys stay below the tags. A track that gains its first insert feeds
// the bus through a new pair, (TrackFader, BusSum), whose delay line starts
// from silence: the one silent block ADR-0012's Consequences accept for a
// chain edit. A track split for its sends alone has no chain edit to
// excuse that, so its TrackFader keeps the TrackOut's id, untagged.
constexpr uint32_t kMaxTrackKey = 0x007FFFFFu;
// An engine track with no model track behind it.
constexpr uint32_t kSpareTrackKeyTag = 0x00800000u;
constexpr uint32_t kTrackSourceIdTag = 0x01000000u;
constexpr uint32_t kInsertChainIdTag = 0x02000000u;
constexpr uint32_t kTrackFaderIdTag = 0x03000000u;

// Sends and aux buses (ADR-0022 §5). A send from engine track `strip`, or
// from engine bus `strip - tracks`, in `slot`, into engine bus `bus`.
struct SendBinding {
    uint32_t strip = 0;
    uint32_t slot = 0;
    uint32_t bus = 0;
};

struct MixLayout {
    // Engine buses, `bus_count` of them: each one's stable key, and their
    // order in the plan, which must be topological over the sends (a bus
    // after every bus that sends to it). Both bus_count entries.
    uint32_t bus_count = 0;
    const uint32_t* bus_keys = nullptr;
    const uint32_t* bus_order = nullptr;
    // Any order; the layout sums them in strip order, then slot order.
    const SendBinding* sends = nullptr;
    uint32_t send_count = 0;
};

struct GraphSize {
    uint32_t nodes = 0;
    uint32_t edges = 0;
};

// What layout_mix_graph() needs for `tracks` tracks: a track with an active
// chain or a send is split, three nodes with a chain and two without.
GraphSize mix_graph_size(uint32_t tracks, const ChainBinding* chains, const MixLayout& mix);

// Stable id tags of an aux bus's nodes, over its key.
constexpr uint32_t kAuxSumIdTag = 0x04000000u;
constexpr uint32_t kAuxFaderIdTag = 0x05000000u;

// layout_track_graph() with sends and aux buses. A track with a send and no
// active chain is TrackSource -> TrackFader; its sends leave from the last
// node before TrackFader, the pre-fader point. Each bus, in mix.bus_order,
// is AuxSum -> AuxFader, its sends leaving from AuxSum. BusSum sums the
// tracks in order, then the buses in plan order. Needs nodes and edges
// sized by mix_graph_size(). A send naming a strip or a bus out of range is
// left out; mix_graph_size() leaves it out too.
void layout_mix_graph(PlaybackPlan& plan, const ChainBinding* chains, const MixLayout& mix);

// layout_monitor_graph() with chains: track t is a TrackOut, or, when
// chains[t].active, TrackSource -> InsertChain -> TrackFader; then BusSum
// and HardwareOut. Edges grouped by to_node, every delay 0 and no delay
// line. Needs nodes and edges sized by graph_nodes()/graph_edges() over the
// active chains. Sets last_insert_node. Scratch buffer = node index. An
// active binding with no channel slot or no node state is laid out
// unbound: kNoChannelSlot, no store, kNoNodeState, latency 0. The session
// passes such a chain through, so its latency is 0 whatever it declared.
void layout_track_graph(PlaybackPlan& plan, const ChainBinding* chains);

// Which delay line serves which (from stable id, to stable id) pair, kept by
// the control thread from one build to the next so an edge keeps its line
// across plans (ADR-0012 §7.3). A line is reassigned only when the pool is
// otherwise full, least recently used first, and never to two edges of one
// plan.
class DelayLinePool {
public:
    explicit DelayLinePool(uint32_t lines = 0);
    uint32_t lines() const { return static_cast<uint32_t>(owner_.size()); }
    // Before a build's first assign().
    void begin_build();
    // The pair's line, or kNoDelayLine when every line is taken by this
    // build (auditor L3: never an index past the pool).
    uint32_t assign(uint64_t pair);
    // The pair a line was last assigned to; 0 if none. For tests.
    uint64_t owner(uint32_t line) const { return line < owner_.size() ? owner_[line] : 0; }

private:
    std::vector<uint64_t> owner_;      // 0: never assigned
    std::vector<uint64_t> last_used_;  // build number
    uint64_t build_ = 0;
};

inline uint64_t edge_pair(uint32_t from_stable_id, uint32_t to_stable_id) {
    return (static_cast<uint64_t>(from_stable_id) << 32) | to_stable_id;
}

struct Compensation {
    uint32_t compensation_frames = 0;   // latency at HardwareOut
    uint32_t shortfall_frames = 0;      // worst edge, frames short of aligned
    uint32_t clamped_edges = 0;         // over max_delay_frames
    uint32_t undelayed_edges = 0;       // no line left in the pool
};

// ADR-0012 §7.3 over a laid-out plan, in its topological order:
// L(n) = max over inputs of L(from) + d(edge), plus n's latency_frames;
// d(edge) = max_in - L(from). An edge needing more than `max_delay_frames`
// is clamped to it, and one for which the pool has no line is left
// undelayed; both are reported as shortfall, never refused. Lines go to the
// edges into BusSum before the sends, and an edge out of an aux bus that no
// send reaches carries silence and takes none. Writes every
// edge's delay_frames and delay_line and the plan's compensation fields.
// `track_shortfall`, if not null, has plan.track_count entries and gets the
// shortfall of each track's edge into the bus.
Compensation compensate(PlaybackPlan& plan, uint32_t max_delay_frames, DelayLinePool& pool,
                        uint32_t* track_shortfall = nullptr);

}  // namespace sonora
