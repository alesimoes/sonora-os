// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginChainRunner without a host (ADR-0012 §7.2): the cases it decides
// itself, before or after PluginClient. A node with no node state plays
// dry; a chain wider than the session's stereo planes is silence, and the
// client is never handed planes the caller lacks; a mono chain's side 1 is
// its side 0. A chain bound to a slot no host was ever assigned to is the
// client's silence, counted as a miss, and begin/end bracket every callback.
// The same runner against real hosts: test_session_plugins.
#include <cstdio>
#include <vector>

#include "sonora/plugin_chain_runner.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

using sonora::PlanNode;
using sonora::PluginChainRunner;
namespace pc = sonora::plugin_client;

constexpr uint32_t kBlock = 64;

struct Buffers {
    std::vector<float> in[2], out[2];
    const float* ip[2];
    float* op[2];
    Buffers() {
        for (int s = 0; s < 2; ++s) {
            in[s].assign(kBlock, 0.f);
            out[s].assign(kBlock, 0.f);
            for (uint32_t i = 0; i < kBlock; ++i) in[s][i] = float(s + 1) * 0.001f * float(i + 1);
            ip[s] = in[s].data();
            op[s] = out[s].data();
        }
    }
    void scribble() {
        for (auto& o : out)
            for (float& v : o) v = 12345.f;
    }
};

pc::ClientConfig config() {
    pc::ClientConfig c;
    c.block_frames = kBlock;
    c.max_channels = 4;
    c.max_stores = 2;
    c.store_slots = 16;
    c.max_nodes = 4;
    return c;
}

int test_no_node_state_plays_dry() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Buffers b;
    b.scribble();
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = 0;
    n.node_state = sonora::kNoNodeState;
    n.channels = 2;
    runner.begin(1);
    runner.run(n, b.ip, b.op, kBlock);
    runner.end();
    CHECK(b.out[0] == b.in[0] && b.out[1] == b.in[1]);
    CHECK(runner.passed_through() == 1);
    CHECK(runner.collected() + runner.missed() + runner.removed() == 0);
    CHECK(client.callbacks() == 1);
    return 0;
}

int test_wider_than_stereo_is_silence() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Buffers b;
    b.scribble();
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = 0;
    n.node_state = 0;
    n.channels = 3;  // a key channel the session's planes cannot carry
    runner.begin(1);
    runner.run(n, b.ip, b.op, kBlock);
    runner.end();
    for (int s = 0; s < 2; ++s)
        for (float v : b.out[s]) CHECK(v == 0.f);
    CHECK(runner.unsupported() == 1);
    CHECK(client.node_status(0).silent_blocks == 0);  // never reached the client
    return 0;
}

// A slot no host was assigned: generation 0 is never published, so the
// client plays silence for it. Mono: side 1 is side 0, not stale.
int test_unassigned_slot_is_a_miss_and_mono_fills_both_sides() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Buffers b;
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = 1;
    n.channel_generation = 0;
    n.param_store = sonora::kNoParamStore;
    n.node_state = 2;
    n.channels = 1;
    for (int cb = 0; cb < 3; ++cb) {
        b.scribble();
        runner.begin(1000 + cb);
        runner.run(n, b.ip, b.op, kBlock);
        runner.end();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(b.out[0][i] == 0.f);
            CHECK(b.out[1][i] == b.out[0][i]);
        }
    }
    CHECK(runner.missed() == 3);
    CHECK(runner.collected() == 0 && runner.removed() == 0);
    CHECK(client.node_status(2).silent_blocks == 3);
    CHECK(client.callbacks() == 3);
    return 0;
}

}  // namespace

int main() {
    if (test_no_node_state_plays_dry()) return 1;
    if (test_wider_than_stereo_is_silence()) return 1;
    if (test_unassigned_slot_is_a_miss_and_mono_fills_both_sides()) return 1;
    std::puts("plugin_chain_runner: ok");
    return 0;
}
