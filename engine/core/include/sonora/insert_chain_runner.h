// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an InsertChain node runs on the audio thread (ADR-0012 §7.2).
//
// engine/plugin_client implements it over its HostChannel slots; engine/core
// only calls it, so the core links no plugin code. engine/core's tests
// implement it as a channel that delays by its declared latency, which is
// how the 2048-sample alignment is checked before any host exists. With no
// runner installed, or for a node bound to no channel slot, the session
// passes the chain's input through.
#pragma once

#include <cstdint>

#include "sonora/playback_plan.h"
#include "sonora/rt.h"

namespace sonora {

class InsertChainRunner {
public:
    // `in` and `out` are the node's input and its scratch buffer, both
    // stereo; a mono chain (node.channels == 1) reads and writes side 0
    // only. Called once per piece of a block the session mixes, in plan
    // order. Must not wait: a host that is not done is a miss, played as
    // the implementation's silence.
    virtual void run(const PlanNode& node, const float* const in[2], float* const out[2],
                     uint32_t frames) noexcept SONORA_RT = 0;

protected:
    // Not owned by the session; never deleted through this type.
    ~InsertChainRunner() = default;
};

}  // namespace sonora
