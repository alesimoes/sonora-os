// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plugin_chain_runner.h"

#include <cstring>

namespace sonora {

void PluginChainRunner::begin(uint64_t now_ns) noexcept SONORA_RT { client_.begin_callback(now_ns); }

void PluginChainRunner::end() noexcept SONORA_RT { client_.end_callback(); }

void PluginChainRunner::run(const PlanNode& node, const float* const in[2], float* const out[2],
                            uint32_t frames) noexcept SONORA_RT {
    const std::size_t bytes = std::size_t(frames) * sizeof(float);
    if (node.node_state == kNoNodeState) {
        std::memset(out[0], 0, bytes);
        std::memset(out[1], 0, bytes);
        bump(no_node_state_);
        return;
    }
    if (node.channels == 0 || node.channels > 2) {
        std::memset(out[0], 0, bytes);
        std::memset(out[1], 0, bytes);
        bump(unsupported_);
        return;
    }
    const plugin_client::InsertChainRef ref{node.channel_slot, node.channel_generation, node.param_store,
                                            node.node_state, node.channels};
    const plugin_client::Outcome o = client_.process_chain(ref, in, out, frames, nullptr, 0);
    if (node.channels == 1) std::memcpy(out[1], out[0], bytes);
    switch (o) {
        case plugin_client::Outcome::Collected: bump(collected_); break;
        case plugin_client::Outcome::Missed: bump(missed_); break;
        case plugin_client::Outcome::Removed: bump(removed_); break;
    }
}

}  // namespace sonora
