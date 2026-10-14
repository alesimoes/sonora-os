// SPDX-License-Identifier: GPL-3.0-or-later
//
// The InsertChainRunner the engine installs: each InsertChain node runs
// through engine/plugin_client's PluginClient, so a plan's chains reach the
// out-of-process hosts (ADR-0012 §7.2, checklist 4).
//
// Here, in engine/core, rather than in engine/plugin_client as
// insert_chain_runner.h first sketched it: ADR-0007's dependency graph
// draws engine/core -> engine/plugin_client and not the reverse, and the
// adapter needs both PlanNode and PluginClient. The core still links no
// plugin SDK and starts no process; it calls the RT half's three entry
// points and nothing else of plugin_client on the audio thread.
//
// Per node:
//   - node_state == kNoNodeState: passed through (insert_chain_runner.h:
//     every bound chain needs a node state; one without it is the
//     builder's slip, played dry and counted, never handed to the client).
//   - channels outside 1..2: silence, counted. The session's scratch
//     buffers are stereo planes, so a wider chain (key channels) cannot be
//     fed from them; PluginClient would index planes the caller lacks.
//   - otherwise process_chain with the whole callback. A mono chain writes
//     side 0; side 1 is set to it, so the scratch buffer never carries a
//     previous node's samples into a fader that reads both.
// No parameter events yet: the session has no path from RtCommand or
// automation to a chain (checklist 5 maps PluginParameterChange onto one).
// Until then every node runs on its store's values.
#pragma once

#include <atomic>
#include <cstdint>

#include "sonora/insert_chain_runner.h"
#include "sonora/plugin_client/plugin_client.h"

namespace sonora {

static_assert(kNoChannelSlot == plugin_client::kNoSlot,
              "a plan's unbound chain is plugin_client's free slot value");
static_assert(kNoParamStore == plugin_client::kNoStore,
              "a plan's chain without a store is plugin_client's kNoStore");

class PluginChainRunner final : public InsertChainRunner {
public:
    // `client` outlives the runner, and the runner every callback that can
    // have loaded it (Session::set_insert_chains).
    explicit PluginChainRunner(plugin_client::PluginClient& client) : client_(client) {}

    void begin(uint64_t now_ns) noexcept SONORA_RT override;
    void run(const PlanNode& node, const float* const in[2], float* const out[2],
             uint32_t frames) noexcept SONORA_RT override;
    void end() noexcept SONORA_RT override;

    // Counters for the control thread, monotonic. One per node run.
    uint64_t collected() const { return collected_.load(std::memory_order_relaxed); }
    uint64_t missed() const { return missed_.load(std::memory_order_relaxed); }
    uint64_t removed() const { return removed_.load(std::memory_order_relaxed); }
    uint64_t passed_through() const { return passed_through_.load(std::memory_order_relaxed); }
    uint64_t unsupported() const { return unsupported_.load(std::memory_order_relaxed); }

private:
    // One writer, the audio thread: load and store, no read-modify-write.
    static void bump(std::atomic<uint64_t>& c) noexcept SONORA_RT {
        c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    plugin_client::PluginClient& client_;
    std::atomic<uint64_t> collected_{0};
    std::atomic<uint64_t> missed_{0};
    std::atomic<uint64_t> removed_{0};
    std::atomic<uint64_t> passed_through_{0};
    std::atomic<uint64_t> unsupported_{0};
};

}  // namespace sonora
