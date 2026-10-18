// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginChainRunner without a host (ADR-0012 §7.2): the cases it decides
// itself, before or after PluginClient. A node with no node state is
// silence; a chain wider than the session's stereo planes is silence, and the
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

// Review of PR #59 (rt-engineer ruling 1): a node bound to a slot, with a
// planned latency, but no node state cannot run on a host. Dry, it would
// reach the bus its latency early; it is silence, counted, and never
// handed to the client. Mono and stereo alike.
int test_no_node_state_is_silence() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Buffers b;
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = 0;
    n.node_state = sonora::kNoNodeState;
    n.latency_frames = 2048;
    for (uint16_t channels = 1; channels <= 2; ++channels) {
        n.channels = channels;
        b.scribble();
        runner.begin(channels);
        runner.run(n, b.ip, b.op, kBlock, nullptr, 0);
        runner.end();
        for (int s = 0; s < 2; ++s)
            for (float v : b.out[s]) CHECK(v == 0.f);
    }
    CHECK(runner.no_node_state() == 2);
    CHECK(runner.collected() + runner.missed() + runner.removed() + runner.unsupported() == 0);
    CHECK(client.node_status(0).silent_blocks == 0);  // never reached the client
    CHECK(client.callbacks() == 2);
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
    n.channels = 3;  // a plan the builder never makes (ChannelMismatch, §7.1)
    runner.begin(1);
    runner.run(n, b.ip, b.op, kBlock, nullptr, 0);
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
        runner.run(n, b.ip, b.op, kBlock, nullptr, 0);
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

// Parameter events reach the client for a node it is handed (a node with no
// store drops them there, counted against the node), and are dropped and
// counted by the runner for a node it keeps from the client.
// skip(): a node not run gets the client's fade of its node state (no
// sample was played yet, so silence), counted on the node; a mono node's
// side 1 is its side 0; a node with no node state, or a silent slot handed
// to run(), is silence and never reaches a host.
int test_skip_fades_through_the_client() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Buffers b;
    b.scribble();
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = 1;
    n.node_state = 1;
    n.channels = 1;
    runner.skip(n, b.op, kBlock);
    for (int s = 0; s < 2; ++s)
        for (float v : b.out[s]) CHECK(v == 0.f);
    CHECK(client.node_status(1).silent_blocks == 1);

    b.scribble();
    PlanNode none = n;
    none.node_state = sonora::kNoNodeState;
    runner.skip(none, b.op, kBlock);
    for (int s = 0; s < 2; ++s)
        for (float v : b.out[s]) CHECK(v == 0.f);

    b.scribble();
    PlanNode silent = n;
    silent.channel_slot = sonora::kSilentChannelSlot;
    const sonora::ChainParamEvent ev[1] = {{0, 0, 0, 1.f}};
    runner.begin(1);
    runner.run(silent, b.ip, b.op, kBlock, ev, 1);
    runner.end();
    for (int s = 0; s < 2; ++s)
        for (float v : b.out[s]) CHECK(v == 0.f);
    CHECK(client.node_status(1).silent_blocks == 2);
    CHECK(runner.events_dropped() == 1);
    CHECK(runner.collected() + runner.missed() + runner.removed() == 0);
    return 0;
}

int test_events_reach_the_client() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Buffers b;
    const sonora::ChainParamEvent ev[3] = {{0, 1, 0, 0.5f}, {0, 2, 0, 0.25f}, {1, 0, 0, 1.f}};
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = 1;
    n.param_store = sonora::kNoParamStore;
    n.node_state = 3;
    n.channels = 1;
    runner.begin(1);
    runner.run(n, b.ip, b.op, kBlock, ev, 3);
    runner.end();
    CHECK(client.node_status(3).events_dropped == 3);
    CHECK(runner.events_dropped() == 0);

    PlanNode dry = n;
    dry.node_state = sonora::kNoNodeState;
    PlanNode wide = n;
    wide.node_state = 0;
    wide.channels = 3;
    runner.begin(2);
    runner.run(dry, b.ip, b.op, kBlock, ev, 2);
    runner.run(wide, b.ip, b.op, kBlock, ev, 1);
    runner.end();
    CHECK(runner.events_dropped() == 3);
    return 0;
}

}  // namespace

int main() {
    if (test_no_node_state_is_silence()) return 1;
    if (test_wider_than_stereo_is_silence()) return 1;
    if (test_unassigned_slot_is_a_miss_and_mono_fills_both_sides()) return 1;
    if (test_events_reach_the_client()) return 1;
    if (test_skip_fades_through_the_client()) return 1;
    std::puts("plugin_chain_runner: ok");
    return 0;
}
