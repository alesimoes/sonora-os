// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginChainRunner with a keyed node (ADR-0058 §2): the key the session
// hands run_keyed() reaches the region's last input planes, after the
// signal, mono and stereo; run() on a keyed node submits a silent key,
// never the signal; a keyed node on a slot whose layout carries the key is
// no shape fault, and one on a slot without it is (the host spawn with
// key_channels is plugin_hosting's follow-up). Under RTSan the runner's
// audio-thread calls are checked as they run here.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sonora/plugin/protocol.h"
#include "sonora/plugin_chain_runner.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

namespace pc = sonora::plugin_client;
namespace plugin = sonora::plugin;
using sonora::PlanNode;
using sonora::PluginChainRunner;

constexpr uint32_t kBlock = 64;

// One channel region and the host's side of it, driven by the test.
class Region {
public:
    explicit Region(uint32_t channels) {
        map_ = plugin::make_param_map(counts_, 1);
        layout_ = plugin::make_layout(channels, kBlock, map_.param_slots);
        bytes_ = plugin::align_up(layout_.region_size, plugin::kCacheLine);
        base_ = std::aligned_alloc(plugin::kCacheLine, bytes_);
        std::memset(base_, 0, bytes_);
        h_ = plugin::init_header(base_, layout_, 48000, kBlock, 1, counts_, 0);
    }
    ~Region() { std::free(base_); }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    pc::ChannelBinding binding() const { return {base_, layout_, map_, kBlock}; }
    const plugin::ParamMap& map() const { return map_; }
    const float* input(uint32_t c) const { return plugin::input_plane(base_, layout_, c); }

    // The host's turn: each output plane is its input plane.
    void complete() {
        const uint64_t block = h_->submitted_block.load(std::memory_order_acquire);
        if (block == h_->completed_block.load()) return;
        for (uint32_t c = 0; c < layout_.channels; ++c) {
            std::memcpy(plugin::output_plane(base_, layout_, c), input(c), kBlock * sizeof(float));
        }
        h_->completed_block.store(block, std::memory_order_release);
    }

private:
    uint32_t counts_[1] = {0};
    plugin::ParamMap map_;
    plugin::Layout layout_;
    std::size_t bytes_ = 0;
    void* base_ = nullptr;
    plugin::ChannelHeader* h_ = nullptr;
};

pc::ClientConfig config() {
    pc::ClientConfig c;
    c.block_frames = kBlock;
    c.max_channels = 2;
    c.max_stores = 1;
    c.store_slots = 16;
    c.max_nodes = 1;
    c.max_chain_channels = 4;
    return c;
}

// A keyed node of `channels` on a fresh slot of `region`.
PlanNode bind(pc::PluginClient& client, const Region& region, uint16_t channels, uint16_t keys) {
    const uint32_t slot = client.acquire_slot();
    client.reset_store(0, region.map(), nullptr, nullptr);
    client.reset_node(0);
    PlanNode n;
    n.kind = sonora::NodeKind::InsertChain;
    n.channel_slot = slot;
    n.channel_generation = client.assign(slot, region.binding());
    n.param_store = 0;
    n.node_state = 0;
    n.channels = channels;
    n.key_channels = keys;
    return n;
}

struct Planes {
    std::vector<float> in[2], key[2], out[2];
    const float* ip[2];
    const float* kp[2];
    float* op[2];
    Planes() {
        for (int s = 0; s < 2; ++s) {
            in[s].assign(kBlock, 0.f);
            key[s].assign(kBlock, 0.f);
            out[s].assign(kBlock, 0.f);
            ip[s] = in[s].data();
            kp[s] = key[s].data();
            op[s] = out[s].data();
        }
    }
    void fill(uint32_t cb) {
        for (int s = 0; s < 2; ++s)
            for (uint32_t i = 0; i < kBlock; ++i) {
                in[s][i] = float(int((cb * 37 + i + 11 * s) % 50) - 25) / 64.f;
                key[s][i] = float(int((cb * 53 + i + 7 * s) % 40) - 20) / 32.f;
            }
    }
};

bool same(const float* a, const float* b) { return std::memcmp(a, b, kBlock * sizeof(float)) == 0; }

int key_reaches_the_last_planes(uint16_t channels) {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Region region(2u * channels);
    const PlanNode n = bind(client, region, channels, channels);
    Planes p;
    for (uint32_t cb = 1; cb <= 4; ++cb) {
        p.fill(cb);
        runner.begin(1000 + cb);
        runner.run_keyed(n, p.ip, p.kp, p.op, kBlock, nullptr, 0);
        runner.end();
        for (uint32_t c = 0; c < channels; ++c) {
            CHECK(same(region.input(c), p.in[c].data()));
            CHECK(same(region.input(channels + c), p.key[c].data()));
        }
        region.complete();
    }
    CHECK(client.node_status(0).shape_faults == 0);
    CHECK(runner.collected() > 0);
    CHECK(runner.unsupported() == 0);
    return 0;
}

// run() has no key to give: the key planes are silence, not the signal.
int run_on_a_keyed_node_is_a_silent_key() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Region region(2);
    const PlanNode n = bind(client, region, 1, 1);
    Planes p;
    p.fill(1);
    runner.begin(1);
    runner.run(n, p.ip, p.op, kBlock, nullptr, 0);
    runner.end();
    CHECK(same(region.input(0), p.in[0].data()));
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(region.input(1)[i] == 0.f);
    CHECK(client.node_status(0).shape_faults == 0);
    return 0;
}

// A slot spawned without the key planes: the node's split is not the
// slot's, so the chain plays silence, counted, and is never run unkeyed.
int keyed_node_on_an_unkeyed_slot_is_a_shape_fault() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Region region(1);
    const PlanNode n = bind(client, region, 1, 1);
    Planes p;
    p.fill(1);
    for (float& v : p.out[0]) v = 12345.f;
    runner.begin(1);
    runner.run_keyed(n, p.ip, p.kp, p.op, kBlock, nullptr, 0);
    runner.end();
    CHECK(client.node_status(0).shape_faults == 1);
    for (int s = 0; s < 2; ++s)
        for (float v : p.out[s]) CHECK(v == 0.f);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(region.input(0)[i] == 0.f);
    return 0;
}

// More key planes than the session's stereo: refused before the client.
int wider_key_is_unsupported() {
    pc::PluginClient client(config());
    PluginChainRunner runner(client);
    Region region(2);
    PlanNode n = bind(client, region, 1, 1);
    n.key_channels = 3;
    Planes p;
    runner.begin(1);
    runner.run_keyed(n, p.ip, p.kp, p.op, kBlock, nullptr, 0);
    runner.end();
    CHECK(runner.unsupported() == 1);
    for (int s = 0; s < 2; ++s)
        for (float v : p.out[s]) CHECK(v == 0.f);
    return 0;
}

}  // namespace

int main() {
    if (key_reaches_the_last_planes(1)) return 1;
    if (key_reaches_the_last_planes(2)) return 1;
    if (run_on_a_keyed_node_is_a_silent_key()) return 1;
    if (keyed_node_on_an_unkeyed_slot_is_a_shape_fault()) return 1;
    if (wider_key_is_unsupported()) return 1;
    std::puts("keyed_chain_runner: ok");
    return 0;
}
