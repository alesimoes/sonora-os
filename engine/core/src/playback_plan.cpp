// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/playback_plan.h"

#include <cstddef>
#include <cstring>
#include <new>

namespace sonora {
namespace {

constexpr std::size_t kAlign = 64;

std::size_t round_up(std::size_t n) { return (n + kAlign - 1) / kAlign * kAlign; }

}  // namespace

PlaybackPlan* create_plan(const PlanSizes& sizes) {
    const std::size_t header = round_up(sizeof(PlaybackPlan));
    const std::size_t tracks = round_up(sizeof(PlanTrack) * sizes.tracks);
    const std::size_t segments = round_up(sizeof(PlanSegment) * sizes.segments);
    const std::size_t nodes = round_up(sizeof(PlanNode) * sizes.nodes);
    const std::size_t edges = round_up(sizeof(PlanEdge) * sizes.edges);
    const std::size_t slots = round_up(sizeof(std::atomic<uint32_t>) * sizes.chunk_slots);
    const std::size_t click = round_up(sizeof(ClickSegment) * sizes.click_segments);
    const std::size_t mix = round_up(sizeof(PlanTrackMix) * sizes.track_mix);
    const std::size_t send_mix = round_up(sizeof(PlanSendMix) * sizes.send_mix);
    const std::size_t auto_points = round_up(sizeof(PlanAutoPoint) * sizes.auto_points);
    const std::size_t plugin_lanes = round_up(sizeof(PlanPluginLane) * sizes.plugin_lanes);
    const std::size_t total = header + tracks + segments + nodes + edges + slots + click + mix +
                              send_mix + auto_points + plugin_lanes;

    void* raw = ::operator new(total, std::align_val_t{kAlign}, std::nothrow);
    if (!raw) return nullptr;
    // Every byte, so every page is faulted in here and not by the audio
    // thread (ADR-0009 §4.3).
    std::memset(raw, 0, total);
    char* const base = static_cast<char*>(raw);
    char* at = base + header;

    auto* plan = new (base) PlaybackPlan();
    plan->tracks = new (at) PlanTrack[sizes.tracks];
    plan->track_count = sizes.tracks;
    at += tracks;
    plan->segments = new (at) PlanSegment[sizes.segments];
    plan->segment_count = sizes.segments;
    at += segments;
    plan->nodes = new (at) PlanNode[sizes.nodes];
    plan->node_count = sizes.nodes;
    at += nodes;
    plan->edges = new (at) PlanEdge[sizes.edges];
    plan->edge_count = sizes.edges;
    at += edges;
    plan->chunk_slots = reinterpret_cast<std::atomic<uint32_t>*>(at);
    for (uint32_t i = 0; i < sizes.chunk_slots; ++i) {
        new (&plan->chunk_slots[i]) std::atomic<uint32_t>(kNoChunk);
    }
    plan->chunk_slot_count = sizes.chunk_slots;
    at += slots;
    plan->click = new (at) ClickSegment[sizes.click_segments];
    plan->click_count = sizes.click_segments;
    at += click;
    plan->track_mix = new (at) PlanTrackMix[sizes.track_mix];
    plan->track_mix_count = sizes.track_mix;
    at += mix;
    plan->send_mix = new (at) PlanSendMix[sizes.send_mix];
    plan->send_mix_count = sizes.send_mix;
    at += send_mix;
    plan->auto_points = new (at) PlanAutoPoint[sizes.auto_points];
    plan->auto_point_count = sizes.auto_points;
    at += auto_points;
    plan->plugin_lanes = new (at) PlanPluginLane[sizes.plugin_lanes];
    plan->plugin_lane_count = sizes.plugin_lanes;
    return plan;
}

void destroy_plan(PlaybackPlan* plan) {
    // Every member type is trivially destructible; the plan is freed whole.
    ::operator delete(plan, std::align_val_t{kAlign});
}

void layout_monitor_graph(PlaybackPlan& plan) {
    const uint32_t tracks = plan.track_count;
    const uint32_t bus = tracks;
    const uint32_t out = tracks + 1;
    for (uint32_t t = 0; t < tracks; ++t) {
        PlanNode& n = plan.nodes[t];
        n.kind = NodeKind::TrackOut;
        n.stable_id = t + 1;
        n.first_input_edge = 0;
        n.input_edge_count = 0;
        n.scratch_buffer = t;
        n.track = t;
        plan.edges[t] = PlanEdge{t, bus, 0, t};
    }
    plan.nodes[bus] = PlanNode{NodeKind::BusSum, kBusSumStableId, 0, tracks, bus, 0};
    plan.nodes[out] = PlanNode{NodeKind::HardwareOut, kHardwareOutStableId, tracks, 1, out, 0};
    plan.edges[tracks] = PlanEdge{bus, out, 0, tracks};
}

}  // namespace sonora
