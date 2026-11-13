// SPDX-License-Identifier: GPL-3.0-or-later
//
// The order the plan lays the aux buses out in (ADR-0022 §5, ADR-0024 §6).
// Control thread only.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/plan_graph.h"

namespace sonora {

// Topological over the sends between buses and the buses' outputs, ties in
// display order: the lowest-numbered bus no remaining bus feeds goes next.
// A send from strip `engine_tracks + b` is from bus b; `bus_outputs` has one
// entry per bus, kNoBus for the Master. False when they loop. Quadratic in
// buses, which are few.
bool order_buses(uint32_t engine_tracks, const std::vector<SendBinding>& sends,
                 const std::vector<uint32_t>& bus_outputs, std::vector<uint32_t>& order);

}  // namespace sonora
