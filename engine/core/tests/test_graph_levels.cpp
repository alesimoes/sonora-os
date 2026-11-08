// SPDX-License-Identifier: GPL-3.0-or-later
//
// The builder's level pass (ADR-0063 §1): edges only go up a level, the
// order is the same for the same graph, and PDC and delay lines are those
// of the plan before the pass.
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "sonora/graph_levels.h"
#include "sonora/plan_graph.h"
#include "sonora/playback_plan.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::ChainBinding;
using sonora::NodeKind;
using sonora::PlanEdge;
using sonora::PlaybackPlan;

constexpr uint32_t kMaxDelay = 1u << 16;
constexpr uint32_t kLines = 64;

ChainBinding chain(uint32_t key, uint32_t slot, uint32_t latency) {
    ChainBinding c;
    c.stable_key = key;
    c.active = true;
    c.channel_slot = slot;
    c.param_store = slot;
    c.node_state = slot;
    c.latency_frames = latency;
    return c;
}

// Four tracks and three buses: track 0 through a 300-frame chain into bus 2,
// which feeds bus 0, an output path; track 1 through a 40-frame chain to the
// Master, sending to bus 1; track 2 plain, sending to bus 1; track 3 plain
// into bus 1, which feeds the Master. The path's source is deeper than
// anything the Master sums, so only the pass's own ordering puts the BusSum
// after it.
struct Mix {
    std::vector<ChainBinding> chains;
    std::vector<uint32_t> track_out{2, sonora::kNoBus, sonora::kNoBus, 1};
    std::vector<uint32_t> bus_out{sonora::kNoBus, sonora::kNoBus, 0};
    std::vector<uint32_t> bus_order{2, 0, 1};
    std::vector<uint16_t> bus_hw{4, sonora::kNoHardware, sonora::kNoHardware};
    std::vector<sonora::SendBinding> sends{{1, 0, 1}, {2, 1, 1}};

    Mix() : chains{chain(11, 0, 300), chain(12, 1, 40), ChainBinding{}, ChainBinding{}} {
        chains[2].stable_key = 13;
        chains[3].stable_key = 14;
    }

    sonora::MixLayout layout() const {
        sonora::MixLayout m;
        m.bus_count = 3;
        m.bus_order = bus_order.data();
        m.sends = sends.data();
        m.send_count = static_cast<uint32_t>(sends.size());
        m.track_outputs = track_out.data();
        m.bus_outputs = bus_out.data();
        m.bus_hardware = bus_hw.data();
        return m;
    }

    PlaybackPlan* plan() const {
        const auto tracks = static_cast<uint32_t>(chains.size());
        const sonora::MixLayout m = layout();
        const sonora::GraphSize g = sonora::mix_graph_size(tracks, chains.data(), m);
        sonora::PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = g.nodes;
        sizes.edges = g.edges;
        PlaybackPlan* p = sonora::create_plan(sizes);
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        sonora::layout_mix_graph(*p, chains.data(), m);
        return p;
    }
};

struct Delay {
    uint32_t frames;
    uint32_t line;
    bool operator==(const Delay& o) const { return frames == o.frames && line == o.line; }
};

// Each edge's delay by its (from, to) stable-id pair, the identity PDC and
// the delay lines go by.
std::map<uint64_t, Delay> delays(const PlaybackPlan& p) {
    std::map<uint64_t, Delay> out;
    for (uint32_t ei = 0; ei < p.edge_count; ++ei) {
        const PlanEdge& e = p.edges[ei];
        const uint64_t pair =
            sonora::edge_pair(p.nodes[e.from_node].stable_id, p.nodes[e.to_node].stable_id);
        out[pair] = Delay{e.delay_frames, e.delay_line};
    }
    return out;
}

// Nodes sorted by level, ranges contiguous and covering every node, and
// every edge from a lower level and an earlier node.
int check_levelled(const PlaybackPlan& p) {
    CHECK(p.level_count > 0);
    CHECK(sonora::plan_level_count(p) == p.level_count);
    CHECK(p.level_first[0] == 0);
    CHECK(p.level_first[p.level_count] == p.node_count);
    for (uint32_t l = 0; l < p.level_count; ++l) {
        uint32_t first = 0;
        uint32_t count = 0;
        sonora::plan_level_range(p, l, first, count);
        CHECK(count > 0);
        for (uint32_t i = first; i < first + count; ++i) CHECK(p.nodes[i].level == l);
    }
    for (uint32_t ni = 0; ni < p.node_count; ++ni) {
        const sonora::PlanNode& node = p.nodes[ni];
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            const PlanEdge& e = p.edges[node.first_input_edge + k];
            CHECK(e.to_node == ni);
            CHECK(e.from_node < ni);
            CHECK(p.nodes[e.from_node].level < node.level);
        }
    }
    return 0;
}

uint32_t find(const PlaybackPlan& p, NodeKind kind) {
    for (uint32_t i = 0; i < p.node_count; ++i) {
        if (p.nodes[i].kind == kind) return i;
    }
    return sonora::kNoNode;
}

int test_edges_only_go_up() {
    const Mix mix;
    PlaybackPlan* p = mix.plan();
    const uint32_t nodes = p->node_count;
    CHECK(sonora::assign_plan_levels(*p));
    CHECK(p->node_count == nodes);
    if (check_levelled(*p)) return 1;
    // Sources at 0: the four tracks' first nodes; the chains next.
    uint32_t first = 0;
    uint32_t count = 0;
    sonora::plan_level_range(*p, 0, first, count);
    CHECK(count == 4);
    sonora::plan_level_range(*p, 1, first, count);
    uint32_t chains = 0;
    for (uint32_t i = first; i < first + count; ++i) {
        if (p->nodes[i].kind == NodeKind::InsertChain) ++chains;
    }
    CHECK(chains == 2);
    // The orderings no edge carries: Master after the path's source, the
    // path after the Master's out, which is in the last level.
    const uint32_t bus = find(*p, NodeKind::BusSum);
    const uint32_t out = find(*p, NodeKind::HardwareOut);
    const uint32_t path = find(*p, NodeKind::PathOut);
    CHECK(bus != sonora::kNoNode && out != sonora::kNoNode && path != sonora::kNoNode);
    const uint32_t path_from = p->edges[p->nodes[path].first_input_edge].from_node;
    CHECK(p->nodes[path_from].level < p->nodes[bus].level);
    CHECK(p->nodes[out].level < p->nodes[path].level);
    CHECK(p->nodes[path].level + 1u == p->level_count);
    // Every node keeps its own scratch buffer.
    std::vector<uint8_t> seen(nodes, 0);
    for (uint32_t i = 0; i < nodes; ++i) {
        CHECK(p->nodes[i].scratch_buffer < nodes && seen[p->nodes[i].scratch_buffer] == 0);
        seen[p->nodes[i].scratch_buffer] = 1;
    }
    sonora::destroy_plan(p);
    return 0;
}

// The pass reorders a node's inputs nowhere: each node keeps its edges, in
// their order, so every sum adds in the same order as before.
int test_summing_order_is_kept() {
    const Mix mix;
    PlaybackPlan* before = mix.plan();
    PlaybackPlan* after = mix.plan();
    CHECK(sonora::assign_plan_levels(*after));
    for (uint32_t ni = 0; ni < before->node_count; ++ni) {
        const sonora::PlanNode& a = before->nodes[ni];
        uint32_t m = sonora::kNoNode;
        for (uint32_t j = 0; j < after->node_count; ++j) {
            if (after->nodes[j].stable_id == a.stable_id) m = j;
        }
        CHECK(m != sonora::kNoNode);
        const sonora::PlanNode& b = after->nodes[m];
        CHECK(b.first_input_edge == a.first_input_edge && b.input_edge_count == a.input_edge_count);
        for (uint32_t k = 0; k < a.input_edge_count; ++k) {
            const PlanEdge& ea = before->edges[a.first_input_edge + k];
            const PlanEdge& eb = after->edges[b.first_input_edge + k];
            CHECK(before->nodes[ea.from_node].stable_id == after->nodes[eb.from_node].stable_id);
            CHECK(ea.send == eb.send);
        }
    }
    sonora::destroy_plan(before);
    sonora::destroy_plan(after);
    return 0;
}

// The same graph laid out from tracks in another order comes out in the
// same order: ties go by stable id, never by where the builder put a node.
int test_order_is_deterministic() {
    const Mix mix;
    PlaybackPlan* a = mix.plan();
    CHECK(sonora::assign_plan_levels(*a));

    // Tracks 2 and 3 swapped, their routing and sends with them.
    Mix swapped;
    std::swap(swapped.chains[2], swapped.chains[3]);
    std::swap(swapped.track_out[2], swapped.track_out[3]);
    swapped.sends = {{1, 0, 1}, {3, 1, 1}};
    PlaybackPlan* b = swapped.plan();
    CHECK(sonora::assign_plan_levels(*b));

    CHECK(a->node_count == b->node_count && a->level_count == b->level_count);
    for (uint32_t i = 0; i < a->node_count; ++i) {
        CHECK(a->nodes[i].stable_id == b->nodes[i].stable_id);
        CHECK(a->nodes[i].level == b->nodes[i].level);
    }
    for (uint32_t l = 0; l <= a->level_count; ++l) CHECK(a->level_first[l] == b->level_first[l]);

    // And a second pass changes nothing.
    PlaybackPlan* c = mix.plan();
    CHECK(sonora::assign_plan_levels(*c));
    CHECK(sonora::assign_plan_levels(*c));
    for (uint32_t i = 0; i < a->node_count; ++i) {
        CHECK(a->nodes[i].stable_id == c->nodes[i].stable_id);
        CHECK(a->nodes[i].level == c->nodes[i].level);
    }
    sonora::destroy_plan(a);
    sonora::destroy_plan(b);
    sonora::destroy_plan(c);
    return 0;
}

// PDC and delay lines go by stable-id pair: the pass after compensate()
// keeps every edge's delay and line, and compensate() after the pass makes
// the same ones, the Master still waiting for the 300-frame path.
int test_pdc_and_lines_unchanged() {
    const Mix mix;
    PlaybackPlan* serial = mix.plan();
    sonora::DelayLinePool serial_pool(kLines);
    const sonora::Compensation want = sonora::compensate(*serial, kMaxDelay, serial_pool);
    CHECK(want.compensation_frames == 300);
    const std::map<uint64_t, Delay> want_delays = delays(*serial);

    PlaybackPlan* after = mix.plan();
    sonora::DelayLinePool after_pool(kLines);
    sonora::compensate(*after, kMaxDelay, after_pool);
    CHECK(sonora::assign_plan_levels(*after));
    CHECK(delays(*after) == want_delays);
    CHECK(after->compensation_frames == want.compensation_frames);

    PlaybackPlan* before = mix.plan();
    CHECK(sonora::assign_plan_levels(*before));
    sonora::DelayLinePool before_pool(kLines);
    const sonora::Compensation got = sonora::compensate(*before, kMaxDelay, before_pool);
    CHECK(got.compensation_frames == want.compensation_frames);
    CHECK(got.shortfall_frames == want.shortfall_frames);
    CHECK(got.undelayed_edges == want.undelayed_edges);
    CHECK(delays(*before) == want_delays);
    for (uint32_t line = 0; line < kLines; ++line) {
        CHECK(before_pool.owner(line) == serial_pool.owner(line));
    }
    sonora::destroy_plan(serial);
    sonora::destroy_plan(after);
    sonora::destroy_plan(before);
    return 0;
}

// last_insert_node, where the runner's block ends, is the last chain the
// new order runs.
int test_last_insert_node_moves() {
    const Mix mix;
    PlaybackPlan* p = mix.plan();
    CHECK(p->last_insert_node != sonora::kNoNode);
    CHECK(sonora::assign_plan_levels(*p));
    CHECK(p->nodes[p->last_insert_node].kind == NodeKind::InsertChain);
    for (uint32_t i = p->last_insert_node + 1; i < p->node_count; ++i) {
        CHECK(p->nodes[i].kind != NodeKind::InsertChain);
    }
    sonora::destroy_plan(p);
    return 0;
}

// A plan the pass cannot level stays one serial level, untouched; an empty
// plan has no level.
int test_refused_plans() {
    const Mix mix;
    PlaybackPlan* p = mix.plan();
    const uint32_t fader = find(*p, NodeKind::TrackFader);
    PlanEdge& in = p->edges[p->nodes[fader].first_input_edge];
    in.from_node = p->node_count - 1;
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < p->node_count; ++i) ids.push_back(p->nodes[i].stable_id);
    CHECK(!sonora::assign_plan_levels(*p));
    CHECK(p->level_count == 0);
    CHECK(sonora::plan_level_count(*p) == 1);
    for (uint32_t i = 0; i < p->node_count; ++i) CHECK(p->nodes[i].stable_id == ids[i]);
    sonora::destroy_plan(p);

    PlaybackPlan* empty = sonora::create_plan(sonora::PlanSizes{});
    CHECK(sonora::assign_plan_levels(*empty));
    CHECK(sonora::plan_level_count(*empty) == 0);
    sonora::destroy_plan(empty);
    return 0;
}

}  // namespace

int main() {
    if (test_edges_only_go_up()) return 1;
    if (test_summing_order_is_kept()) return 1;
    if (test_order_is_deterministic()) return 1;
    if (test_pdc_and_lines_unchanged()) return 1;
    if (test_last_insert_node_moves()) return 1;
    if (test_refused_plans()) return 1;
    std::printf("graph_levels: ok\n");
    return 0;
}
