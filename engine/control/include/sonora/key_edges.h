// SPDX-License-Identifier: GPL-3.0-or-later
//
// Side-chain key edges in the playback plan (ADR-0058 items 2 and 3): an
// InsertChain node whose chain has a keyed insert gets a second input edge,
// from its key bus's AuxSum, and key_channels equal to its own channels.
// The plan carries the key as an edge so compensate() aligns signal and key
// with the arithmetic it already has (ADR-0012 §7.3).
//
// Control thread only: it allocates.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/plan_graph.h"
#include "sonora/playback_plan.h"

namespace sonora {

// Per engine track: unkeyed, keyed by a bus the document no longer has, or
// the engine bus index whose AuxSum keys its chain.
constexpr uint32_t kUnkeyed = 0xFFFFFFFFu;
constexpr uint32_t kMissingKeyBus = 0xFFFFFFFEu;

// What each engine track's chain is keyed by. Only a chain the layout binds
// to a host is keyed: an unbound chain passes its input through and a
// silent one plays silence, so neither has a key to hear. `bus_index` maps
// a model bus to its engine bus.
std::vector<uint32_t> chain_keys(const std::vector<std::shared_ptr<const model::TrackEdit>>& tracks,
                                 const std::vector<ChainBinding>& bindings,
                                 const std::unordered_map<model::Id, uint32_t>& bus_index);

// The edges the plan needs over mix_graph_size(): one per chain keyed by a
// bus that exists.
uint32_t key_edge_count(const std::vector<uint32_t>& keys);

struct KeyEdgeResult {
    uint32_t edges = 0;
    // Chains keyed by a bus that is gone: their key planes stay silent and
    // the chain stays keyed (ADR-0058, rejected alternative 5).
    uint32_t missing = 0;
};

// After layout_mix_graph(), into a plan sized with key_edge_count() more
// edges, and before compensate(): gives each keyed chain its key_channels
// and its second input edge, then reorders the nodes so every edge goes
// forward again (a chain now waits for its key bus's sum). With no key the
// plan is left exactly as laid out. False, with `error`, when the key edges
// close a feedback loop, which the model refuses but an old file may carry:
// the plan is not built, as for a loop through the sends.
bool add_key_edges(PlaybackPlan& plan, const std::vector<uint32_t>& keys, KeyEdgeResult& result,
                   std::string& error);

}  // namespace sonora
