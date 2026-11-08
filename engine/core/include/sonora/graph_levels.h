// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// The builder's level pass (ADR-0063 §1). Control thread only: it allocates.

#include "sonora/playback_plan.h"

namespace sonora {

// Gives every node of a laid-out plan its level (its longest path from a
// source) and re-sorts the nodes by level, then stable id, then their old
// place, so the same graph always comes out in the same order. Fills
// level_first / level_count and moves last_insert_node to the last
// InsertChain of the new order.
//
// Edges keep their indices: each node's input range stays where it was and
// only from_node / to_node are renumbered, so delay frames, delay lines (by
// stable-id pair) and every node's summing order are unchanged, and running
// compensate() before or after this gives the same plan.
//
// Two orderings compensate() and the mix rely on are kept as dependencies
// though no edge carries them: a PathOut comes after the HardwareOut (it
// takes the Master's latency, and writes the device after it), and the
// BusSum after every PathOut's source (the Master waits for the slowest
// path, ADR-0060).
//
// Returns false, and leaves the plan as it was with level_count 0 (one
// serial level), when the plan is not laid out forward (an edge out of
// range, not grouped by to_node, or not from an earlier node) or has more
// levels than PlanNode::level holds.
bool assign_plan_levels(PlaybackPlan& plan);

}  // namespace sonora
