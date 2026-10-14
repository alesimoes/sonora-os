// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plan_graph.h"

#include <algorithm>

namespace sonora {

void layout_track_graph(PlaybackPlan& plan, const ChainBinding* chains) {
    const uint32_t tracks = plan.track_count;
    uint32_t n = 0;
    uint32_t e = 0;
    // The node feeding the bus from each track, for the BusSum's edges.
    std::vector<uint32_t> last(tracks);
    plan.last_insert_node = kNoNode;
    for (uint32_t t = 0; t < tracks; ++t) {
        const uint32_t id = chains && chains[t].stable_key != 0 ? chains[t].stable_key : t + 1;
        if (!chains || !chains[t].active) {
            plan.nodes[n] = PlanNode{};
            plan.nodes[n].kind = NodeKind::TrackOut;
            plan.nodes[n].stable_id = id;
            plan.nodes[n].scratch_buffer = n;
            plan.nodes[n].track = t;
            last[t] = n++;
            continue;
        }
        const ChainBinding& c = chains[t];
        const uint16_t channels = plan.tracks[t].channels;
        const uint32_t source = n;
        PlanNode& s = plan.nodes[n];
        s = PlanNode{};
        s.kind = NodeKind::TrackSource;
        s.stable_id = kTrackSourceIdTag | id;
        s.scratch_buffer = n;
        s.track = t;
        s.channels = channels;
        ++n;

        const uint32_t chain = n;
        PlanNode& ch = plan.nodes[n];
        ch = PlanNode{};
        ch.kind = NodeKind::InsertChain;
        ch.stable_id = kInsertChainIdTag | id;
        ch.first_input_edge = e;
        ch.input_edge_count = 1;
        ch.scratch_buffer = n;
        ch.track = t;
        // The builder decides "unbound" (PR #59, rt-engineer ruling 1): a
        // binding with no slot or no node state cannot run on a host, so
        // the session passes it through, and a pass-through has no latency.
        // Laid out with its latency, compensate() would delay every other
        // track for a delay this one never has, and it would arrive early.
        const bool bound = c.channel_slot != kNoChannelSlot && c.node_state != kNoNodeState;
        if (bound) {
            ch.channel_slot = c.channel_slot;
            ch.channel_generation = c.channel_generation;
            ch.param_store = c.param_store;
            ch.node_state = c.node_state;
            ch.latency_frames = c.latency_frames;
        }
        ch.channels = channels;
        ch.chain_id = c.chain_id;
        plan.edges[e++] = PlanEdge{source, chain, 0, kNoDelayLine};
        plan.last_insert_node = chain;
        ++n;

        PlanNode& f = plan.nodes[n];
        f = PlanNode{};
        f.kind = NodeKind::TrackFader;
        f.stable_id = kTrackFaderIdTag | id;
        f.first_input_edge = e;
        f.input_edge_count = 1;
        f.scratch_buffer = n;
        f.track = t;
        plan.edges[e++] = PlanEdge{chain, n, 0, kNoDelayLine};
        last[t] = n++;
    }
    const uint32_t bus = n++;
    const uint32_t out = n++;
    plan.nodes[bus] = PlanNode{NodeKind::BusSum, kBusSumStableId, e, tracks, bus, 0};
    for (uint32_t t = 0; t < tracks; ++t) plan.edges[e++] = PlanEdge{last[t], bus, 0, kNoDelayLine};
    plan.nodes[out] = PlanNode{NodeKind::HardwareOut, kHardwareOutStableId, e, 1, out, 0};
    plan.edges[e++] = PlanEdge{bus, out, 0, kNoDelayLine};
}

DelayLinePool::DelayLinePool(uint32_t lines) : owner_(lines, 0), last_used_(lines, 0) {}

void DelayLinePool::begin_build() { ++build_; }

uint32_t DelayLinePool::assign(uint64_t pair) {
    const auto count = static_cast<uint32_t>(owner_.size());
    for (uint32_t i = 0; i < count; ++i) {
        if (owner_[i] == pair) {
            last_used_[i] = build_;
            return i;
        }
    }
    uint32_t best = kNoDelayLine;
    for (uint32_t i = 0; i < count; ++i) {
        if (last_used_[i] == build_) continue;
        if (best == kNoDelayLine || last_used_[i] < last_used_[best]) best = i;
    }
    if (best == kNoDelayLine) return kNoDelayLine;
    owner_[best] = pair;
    last_used_[best] = build_;
    return best;
}

Compensation compensate(PlaybackPlan& plan, uint32_t max_delay_frames, DelayLinePool& pool,
                        uint32_t* track_shortfall) {
    Compensation result;
    pool.begin_build();
    if (track_shortfall) std::fill(track_shortfall, track_shortfall + plan.track_count, 0u);
    // 64-bit: a chain of chains could in principle sum past 2^32 frames.
    std::vector<uint64_t> latency(plan.node_count, 0);
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        uint64_t max_in = 0;
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            max_in = std::max(max_in, latency[plan.edges[node.first_input_edge + k].from_node]);
        }
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            PlanEdge& edge = plan.edges[node.first_input_edge + k];
            const uint64_t need = max_in - latency[edge.from_node];
            uint64_t applied = std::min<uint64_t>(need, max_delay_frames);
            if (applied < need) ++result.clamped_edges;
            uint32_t line = kNoDelayLine;
            if (applied > 0) {
                line = pool.assign(edge_pair(plan.nodes[edge.from_node].stable_id, node.stable_id));
                if (line == kNoDelayLine) {
                    applied = 0;
                    ++result.undelayed_edges;
                }
            }
            edge.delay_frames = static_cast<uint32_t>(applied);
            edge.delay_line = line;
            const auto short_by = static_cast<uint32_t>(
                std::min<uint64_t>(need - applied, 0xFFFFFFFFu));
            result.shortfall_frames = std::max(result.shortfall_frames, short_by);
            const PlanNode& from = plan.nodes[edge.from_node];
            const bool track_edge = from.kind == NodeKind::TrackOut || from.kind == NodeKind::TrackFader;
            if (track_shortfall && track_edge && from.track < plan.track_count) {
                track_shortfall[from.track] = std::max(track_shortfall[from.track], short_by);
            }
        }
        latency[ni] = max_in + (node.kind == NodeKind::InsertChain ? node.latency_frames : 0);
        if (node.kind == NodeKind::HardwareOut) {
            result.compensation_frames =
                static_cast<uint32_t>(std::min<uint64_t>(latency[ni], 0xFFFFFFFFu));
        }
    }
    plan.compensation_frames = result.compensation_frames;
    plan.compensation_shortfall_frames = result.shortfall_frames;
    plan.undelayed_edges = result.undelayed_edges;
    return result;
}

}  // namespace sonora
