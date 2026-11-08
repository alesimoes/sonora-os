// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/graph_levels.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace sonora {

namespace {

constexpr uint32_t kMaxLevel = std::numeric_limits<uint16_t>::max();

// Every input range in bounds, every edge in it pointing at its node, and
// coming from an earlier one: the order one forward pass can level.
bool laid_out_forward(const PlaybackPlan& plan) {
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        if (node.first_input_edge > plan.edge_count ||
            node.input_edge_count > plan.edge_count - node.first_input_edge) {
            return false;
        }
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            const PlanEdge& edge = plan.edges[node.first_input_edge + k];
            if (edge.to_node != ni || edge.from_node >= ni) return false;
        }
    }
    return true;
}

// The source of PathOut `pi`, or kNoNode.
uint32_t path_source(const PlaybackPlan& plan, uint32_t pi) {
    const PlanNode& path = plan.nodes[pi];
    if (path.input_edge_count == 0) return kNoNode;
    return plan.edges[path.first_input_edge].from_node;
}

// Longest path from a source, in plan order (laid_out_forward() held). The
// extra terms are compensate()'s orderings that no edge carries; see the
// header. Empty when a level does not fit PlanNode::level.
std::vector<uint32_t> node_levels(const PlaybackPlan& plan) {
    const uint32_t count = plan.node_count;
    std::vector<uint32_t> level(count, 0);
    // One past the highest HardwareOut so far: a PathOut's floor.
    uint32_t after_out = 0;
    for (uint32_t ni = 0; ni < count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        uint32_t l = 0;
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            l = std::max(l, level[plan.edges[node.first_input_edge + k].from_node] + 1);
        }
        if (node.kind == NodeKind::PathOut) l = std::max(l, after_out);
        if (node.kind == NodeKind::BusSum) {
            // As compensate() scans: the PathOuts after it whose source is
            // before it.
            for (uint32_t pi = ni + 1; pi < count; ++pi) {
                if (plan.nodes[pi].kind != NodeKind::PathOut) continue;
                const uint32_t from = path_source(plan, pi);
                if (from < ni) l = std::max(l, level[from] + 1);
            }
        }
        if (l > kMaxLevel) return {};
        level[ni] = l;
        if (node.kind == NodeKind::HardwareOut) after_out = std::max(after_out, l + 1);
    }
    return level;
}

}  // namespace

bool assign_plan_levels(PlaybackPlan& plan) {
    const uint32_t count = plan.node_count;
    plan.level_count = 0;
    if (count == 0) return true;
    if (plan.level_first == nullptr || !laid_out_forward(plan)) return false;
    const std::vector<uint32_t> level = node_levels(plan);
    if (level.empty()) return false;

    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    // Stable: equal level and stable id keep their old order.
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (level[a] != level[b]) return level[a] < level[b];
        return plan.nodes[a].stable_id < plan.nodes[b].stable_id;
    });

    std::vector<uint32_t> new_index(count);
    for (uint32_t i = 0; i < count; ++i) new_index[order[i]] = i;
    const std::vector<PlanNode> old(plan.nodes, plan.nodes + count);
    for (uint32_t i = 0; i < count; ++i) {
        plan.nodes[i] = old[order[i]];
        plan.nodes[i].level = static_cast<uint16_t>(level[order[i]]);
    }
    for (uint32_t ei = 0; ei < plan.edge_count; ++ei) {
        PlanEdge& edge = plan.edges[ei];
        if (edge.from_node < count) edge.from_node = new_index[edge.from_node];
        if (edge.to_node < count) edge.to_node = new_index[edge.to_node];
    }
    if (plan.last_insert_node != kNoNode) {
        for (uint32_t i = 0; i < count; ++i) {
            if (plan.nodes[i].kind == NodeKind::InsertChain) plan.last_insert_node = i;
        }
    }

    // Sorted by level, so each level's first node is where the level rises.
    const uint32_t levels = plan.nodes[count - 1].level + 1u;
    uint32_t at = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        while (at < count && plan.nodes[at].level < l) ++at;
        plan.level_first[l] = at;
    }
    plan.level_first[levels] = count;
    plan.level_count = levels;
    return true;
}

}  // namespace sonora
