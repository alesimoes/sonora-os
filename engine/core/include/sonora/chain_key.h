// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the session needs to play a keyed InsertChain node (ADR-0058 §2):
// the node's second input edge is its key, from an aux bus's AuxSum, and
// its first, the signal, is delayed by what compensate() asks so that
// signal and key reach the chain aligned. Audio thread.
#pragma once

#include <cstdint>

#include "sonora/playback_plan.h"
#include "sonora/rt.h"

namespace sonora {

// How late an InsertChain node's output is against the timeline: what its
// signal edge waits for a key bus that is late, plus the chain's own
// latency. The lanes and the sends after it follow the audio they scale.
inline uint32_t chain_lag(const PlaybackPlan& plan, const PlanNode& chain) noexcept SONORA_RT {
    const uint32_t wait = chain.input_edge_count != 0 ? plan.edges[chain.first_input_edge].delay_frames : 0;
    return wait + chain.latency_frames;
}

// The key edge's source, or kNoNode for an unkeyed node or one whose key
// bus is gone (a silent key).
inline uint32_t key_source(const PlaybackPlan& plan, const PlanNode& chain) noexcept SONORA_RT {
    if (chain.key_channels == 0 || chain.input_edge_count < 2) return kNoNode;
    return plan.edges[chain.first_input_edge + 1].from_node;
}

// A mono chain keyed by a stereo bus hears each side at half.
inline void mono_key(const float* left, const float* right, float* out, uint32_t n) noexcept SONORA_RT {
    for (uint32_t i = 0; i < n; ++i) out[i] = 0.5f * (left[i] + right[i]);
}

}  // namespace sonora
