// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/key_edges.h"

#include <algorithm>
#include <functional>
#include <queue>

namespace sonora {

namespace {

bool bound(const ChainBinding& b) {
    return b.active && b.channel_slot != kNoChannelSlot && b.channel_slot != kSilentChannelSlot &&
           b.node_state != kNoNodeState;
}

// The AuxSum node of each engine bus the plan lays out.
std::unordered_map<uint32_t, uint32_t> aux_sums(const PlaybackPlan& plan) {
    std::unordered_map<uint32_t, uint32_t> sums;
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        if (plan.nodes[ni].kind == NodeKind::AuxSum) sums.emplace(plan.nodes[ni].track, ni);
    }
    return sums;
}

// Each node's edges again, with its key edge after its signal edge, so the
// edges stay grouped by to_node and the signal stays the first input.
void insert_key_edges(PlaybackPlan& plan, const std::vector<uint32_t>& key_from) {
    std::vector<PlanEdge> edges;
    edges.reserve(plan.edge_count);
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        PlanNode& node = plan.nodes[ni];
        const auto first = static_cast<uint32_t>(edges.size());
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            edges.push_back(plan.edges[node.first_input_edge + k]);
        }
        if (key_from[ni] != kNoNode) {
            edges.push_back(PlanEdge{key_from[ni], ni, 0, kNoDelayLine});
            ++node.input_edge_count;
        }
        node.first_input_edge = first;
    }
    std::copy(edges.begin(), edges.end(), plan.edges);
    plan.edge_count = static_cast<uint32_t>(edges.size());
}

// Kahn's order, the lowest laid-out index first among the ready: a plan
// already in order keeps it, and a chain moves just past its key bus's sum.
// Empty when the edges loop.
std::vector<uint32_t> forward_order(const PlaybackPlan& plan) {
    const uint32_t count = plan.node_count;
    std::vector<uint32_t> waiting(count, 0);
    std::vector<std::vector<uint32_t>> next(count);
    for (uint32_t ni = 0; ni < count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            next[plan.edges[node.first_input_edge + k].from_node].push_back(ni);
            ++waiting[ni];
        }
    }
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
    for (uint32_t ni = 0; ni < count; ++ni) {
        if (waiting[ni] == 0) ready.push(ni);
    }
    std::vector<uint32_t> order;
    order.reserve(count);
    while (!ready.empty()) {
        const uint32_t ni = ready.top();
        ready.pop();
        order.push_back(ni);
        for (uint32_t to : next[ni]) {
            if (--waiting[to] == 0) ready.push(to);
        }
    }
    if (order.size() != count) order.clear();
    return order;
}

// Scratch buffers stay with their node: each is still the node's own.
void renumber(PlaybackPlan& plan, const std::vector<uint32_t>& order) {
    const uint32_t count = plan.node_count;
    std::vector<uint32_t> new_index(count);
    for (uint32_t i = 0; i < count; ++i) new_index[order[i]] = i;
    const std::vector<PlanNode> old(plan.nodes, plan.nodes + count);
    for (uint32_t i = 0; i < count; ++i) plan.nodes[i] = old[order[i]];
    for (uint32_t ei = 0; ei < plan.edge_count; ++ei) {
        PlanEdge& edge = plan.edges[ei];
        edge.from_node = new_index[edge.from_node];
        edge.to_node = new_index[edge.to_node];
    }
    plan.last_insert_node = kNoNode;
    for (uint32_t i = 0; i < count; ++i) {
        if (plan.nodes[i].kind == NodeKind::InsertChain) plan.last_insert_node = i;
    }
}

}  // namespace

std::vector<uint32_t> chain_keys(const std::vector<std::shared_ptr<const model::TrackEdit>>& tracks,
                                 const std::vector<ChainBinding>& bindings,
                                 const std::unordered_map<model::Id, uint32_t>& bus_index) {
    std::vector<uint32_t> keys(bindings.size(), kUnkeyed);
    for (std::size_t ti = 0; ti < tracks.size() && ti < bindings.size(); ++ti) {
        if (!bound(bindings[ti])) continue;
        // The model keys at most one insert per chain; the host's key is
        // per chain (ADR-0058 §4).
        for (const model::Insert& in : tracks[ti]->inserts) {
            if (!in.active || in.key_bus == 0) continue;
            const auto it = bus_index.find(in.key_bus);
            keys[ti] = it == bus_index.end() ? kMissingKeyBus : it->second;
            break;
        }
    }
    return keys;
}

uint32_t key_edge_count(const std::vector<uint32_t>& keys) {
    uint32_t n = 0;
    for (uint32_t k : keys) {
        if (k != kUnkeyed && k != kMissingKeyBus) ++n;
    }
    return n;
}

bool add_key_edges(PlaybackPlan& plan, const std::vector<uint32_t>& keys, KeyEdgeResult& result,
                   std::string& error) {
    result = KeyEdgeResult{};
    const std::unordered_map<uint32_t, uint32_t> sums = aux_sums(plan);
    std::vector<uint32_t> key_from(plan.node_count, kNoNode);
    uint32_t wanted = 0;
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        PlanNode& node = plan.nodes[ni];
        if (node.kind != NodeKind::InsertChain || node.track >= plan.track_count) continue;
        const uint32_t t = plan.tracks[node.track].engine_track;
        if (t >= keys.size() || keys[t] == kUnkeyed) continue;
        // Keyed whatever happens to the bus: a key that is gone is a silent
        // key, never an unkeyed chain that listens to itself.
        node.key_channels = node.channels;
        const auto sum = keys[t] == kMissingKeyBus ? sums.end() : sums.find(keys[t]);
        if (sum == sums.end()) {
            ++result.missing;
            continue;
        }
        key_from[ni] = sum->second;
        ++wanted;
    }
    if (wanted == 0) return true;
    // The layout fills what mix_graph_size() asked for; the builder sized
    // the plan with key_edge_count() more.
    uint32_t laid = 0;
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) laid += plan.nodes[ni].input_edge_count;
    if (laid + wanted > plan.edge_count) {
        error = "the plan has room for " + std::to_string(plan.edge_count) + " edges and its keys need " +
                std::to_string(laid + wanted) + "; the plan is not built";
        return false;
    }
    insert_key_edges(plan, key_from);
    const std::vector<uint32_t> order = forward_order(plan);
    if (order.empty()) {
        error = "a side-chain key and the aux buses make a feedback loop; the plan is not built";
        return false;
    }
    renumber(plan, order);
    result.edges = wanted;
    return true;
}

}  // namespace sonora
