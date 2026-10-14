// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an InsertChain node runs on the audio thread (ADR-0012 §7.2).
//
// PluginChainRunner (plugin_chain_runner.h) implements it over
// engine/plugin_client's PluginClient, in engine/core because ADR-0007 draws
// the edge core -> plugin_client and not the reverse; the core still links
// no plugin SDK and starts no host. engine/core's tests
// implement it as a channel that delays by its declared latency, which is
// how the 2048-sample alignment is checked before any host exists. A node
// bound to no channel slot (laid out with latency 0) passes the chain's
// input through. A bound node the session does not run (no runner
// installed, or a callback the runner does not take) plays silence,
// counted in Session::unrun_chain_blocks(): the plan delays every other
// path by its latency, so its dry input would be early.
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
// none of the three for it; its bound chains play silence, and
// Session::chain_bypassed_blocks() counts it. A zero-frame callback calls
// none of the three and is not counted: it has nothing to mix, so end()
// would never come.
//
// PluginChainRunner, the adapter over engine/plugin_client (PR #49), is direct:
//   begin(now_ns)  -> PluginClient::begin_callback(now_ns)
//   run(node, ...) -> PluginClient::process_chain({node.channel_slot,
//                     node.channel_generation, node.param_store,
//                     node.node_state, node.channels}, in, out, frames,
//                     events, count)
// The events are the parameter changes the control thread posted for the
// node's param_store since the last callback (Session::post_plugin_param;
// ADR-0012 §5.1: the audio thread is the parameter ring's only producer),
// in the order they were posted, each at offset 0.
//   end()          -> PluginClient::end_callback()
// and engine/control's ChainResolver fills ChainBinding::latency_frames from
// PluginClient::latency_frames(slot), with the slot, generation, store and
// node state the supervisor assigned. kNoChannelSlot is plugin_client's
// kNoSlot and kNoParamStore its kNoStore (both 0xFFFFFFFF). InsertChainRef
// has no "none" for node_state: every bound chain needs a valid one, so the
// resolver assigns it per chain stable id, as a delay line is kept.
// "Unbound" is decided by the builder, not the runner: layout_track_graph()
// lays out a binding with no slot or no node state as kNoChannelSlot with
// latency 0, and the session passes it through, aligned. A node that still
// reaches run() at kNoNodeState comes from a plan the builder did not make;
// its planned latency cannot be honoured, so the adapter plays silence,
// counted, never the dry input (which would arrive that latency early) and
// never process_chain.
#pragma once

#include <cstdint>

#include "sonora/playback_plan.h"
#include "sonora/plugin_client/plugin_client.h"
#include "sonora/rt.h"

namespace sonora {

// A parameter change for one chain: dense index (or kBypassParam) and
// format-native value, as plugin_client takes it, so the session's array
// reaches process_chain without a copy.
using ChainParamEvent = plugin_client::ParamEvent;

class InsertChainRunner {
public:
    // Before the callback's first InsertChain node. `now_ns` is
    // CLOCK_MONOTONIC (std::chrono::steady_clock) at the period's start, as
    // the backend stamped it (AudioBlock::callback_ns); the session reads
    // no clock.
    virtual void begin(uint64_t now_ns) noexcept SONORA_RT = 0;
    // `in` and `out` are the node's input and its scratch buffer, both
    // stereo planes; a mono chain (node.channels == 1) reads and writes
    // side 0 only. Must not wait: a host that is not done is a miss,
    // played as the implementation's silence. `events`: this callback's
    // parameter changes for node.param_store, `event_count` of them (0 with
    // `events` null when there are none).
    virtual void run(const PlanNode& node, const float* const in[2], float* const out[2],
                     uint32_t frames, const ChainParamEvent* events,
                     uint32_t event_count) noexcept SONORA_RT = 0;
    // After the last InsertChain node (§7.2 step 3: the hosts' tick).
    virtual void end() noexcept SONORA_RT = 0;
    // The one callback size this runner takes, or 0 for any up to
    // PlaybackConfig::max_block_frames. A callback of another size calls
    // none of begin, run and end: its bound chains play silence, counted in
    // Session::chain_bypassed_blocks(). PluginChainRunner's is the client's
    // block_frames, which contracts/plugin requires of every block.
    virtual uint32_t block_frames() const noexcept SONORA_RT = 0;

protected:
    // Not owned by the session; never deleted through this type.
    ~InsertChainRunner() = default;
};

}  // namespace sonora
