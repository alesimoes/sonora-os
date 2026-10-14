// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an InsertChain node runs on the audio thread (ADR-0012 §7.2).
//
// PluginChainRunner (plugin_chain_runner.h) implements it over
// engine/plugin_client's PluginClient, in engine/core because ADR-0007 draws
// the edge core -> plugin_client and not the reverse; the core still links
// no plugin SDK and starts no host. engine/core's tests
// implement it as a channel that delays by its declared latency, which is
// how the 2048-sample alignment is checked before any host exists. With no
// runner installed, or for a node bound to no channel slot, the session
// passes the chain's input through.
//
// The session's call sequence, once per callback, only when the plan has an
// InsertChain node:
//
//     begin(now_ns);
//     run(node, in, out, nframes);   // each InsertChain node, plan order
//     end();                         // right after plan.last_insert_node
//
// run() sees the whole callback, frames == nframes, once per node: the
// depth-1 protocol submits one block per callback. A callback longer than
// PlaybackConfig::max_block_frames is mixed in pieces, so the session calls
// none of the three for it; every chain passes through, and
// Session::chain_bypassed_blocks() counts it.
//
// PluginChainRunner, the adapter over engine/plugin_client (PR #49), is direct:
//   begin(now_ns)  -> PluginClient::begin_callback(now_ns)
//   run(node, ...) -> PluginClient::process_chain({node.channel_slot,
//                     node.channel_generation, node.param_store,
//                     node.node_state, node.channels}, in, out, frames,
//                     events, count)
//   end()          -> PluginClient::end_callback()
// and engine/control's ChainResolver fills ChainBinding::latency_frames from
// PluginClient::latency_frames(slot), with the slot, generation, store and
// node state the supervisor assigned. kNoChannelSlot is plugin_client's
// kNoSlot and kNoParamStore its kNoStore (both 0xFFFFFFFF). InsertChainRef
// has no "none" for node_state: every bound chain needs a valid one, so the
// resolver assigns it per chain stable id, as a delay line is kept, and a
// node still at kNoNodeState is passed through by the adapter rather than
// handed to process_chain.
#pragma once

#include <cstdint>

#include "sonora/playback_plan.h"
#include "sonora/rt.h"

namespace sonora {

class InsertChainRunner {
public:
    // Before the callback's first InsertChain node. `now_ns` is
    // CLOCK_MONOTONIC (std::chrono::steady_clock), read once per callback.
    virtual void begin(uint64_t now_ns) noexcept SONORA_RT = 0;
    // `in` and `out` are the node's input and its scratch buffer, both
    // stereo planes; a mono chain (node.channels == 1) reads and writes
    // side 0 only. Must not wait: a host that is not done is a miss,
    // played as the implementation's silence.
    virtual void run(const PlanNode& node, const float* const in[2], float* const out[2],
                     uint32_t frames) noexcept SONORA_RT = 0;
    // After the last InsertChain node (§7.2 step 3: the hosts' tick).
    virtual void end() noexcept SONORA_RT = 0;

protected:
    // Not owned by the session; never deleted through this type.
    ~InsertChainRunner() = default;
};

}  // namespace sonora
