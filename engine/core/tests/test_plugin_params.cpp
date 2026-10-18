// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plugin parameter changes from the control thread to a chain (ADR-0012
// §5.1, checklist 5): Session::post_plugin_param queues an RtCommand, the
// audio thread drains it at the start of the next callback and hands it to
// the InsertChain node whose plan names that parameter store, in the order
// posted, once. The audio thread stays the parameter ring's only producer.
//
//   routed          each event reaches the node with its store, in order,
//                   offset 0, in the next callback only; another node gets
//                   none
//   unrouted        a store no bound node of the plan names, a callback the
//                   session mixes in pieces, a plan with no chain: dropped
//                   and counted, never held for a later plan
//   refused         no store, a non-finite value, a full queue: refused at
//                   the post, and the queue takes more once drained
//   locate_apart    a full parameter queue never holds a locate back
//
// And two items of the PR #59 reviews, on the same path: the runner's
// now_ns is the backend's callback_ns (the callback reads no clock), and a
// runner with a fixed block size runs only callbacks of that size.
//
// No host: a runner that records what it is handed. engine/core's
// test_session_plugins (scenario params) carries a change to a real one.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"

using playback_test::heap_untouched;
using playback_test::reset_heap_counts;
using playback_test::t_counting;

namespace {

using sonora::ChainBinding;
using sonora::ChainParamEvent;
using sonora::DelayLinePool;
using sonora::PlaybackPlan;
using sonora::PlanNode;
using sonora::Session;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kCapacity = 16;  // the session rounds it to a power of two

// What each run() was handed, by the node's store, in call order.
struct Seen {
    uint32_t store;
    uint64_t callback;
    ChainParamEvent event;
};

class RecordingRunner final : public sonora::InsertChainRunner {
public:
    explicit RecordingRunner(uint32_t fixed = 0) : fixed_(fixed) {}
    void begin(uint64_t now_ns) noexcept SONORA_RT override {
        ++callback;
        began_at = now_ns;
    }
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return fixed_; }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const ChainParamEvent* events, uint32_t event_count) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        if (event_count != 0 && events == nullptr) ++bad_calls;
        for (uint32_t i = 0; i < event_count && count < seen.size(); ++i)
            seen[count++] = Seen{node.param_store, callback, events[i]};
    }

    // Sized before any callback: run() never grows it.
    std::vector<Seen> seen = std::vector<Seen>(256);
    std::size_t count = 0;
    uint64_t callback = 0;
    uint64_t began_at = 0;
    long bad_calls = 0;

private:
    uint32_t fixed_;
};

struct Rig {
    std::unique_ptr<Session> session;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];

    explicit Rig(int tracks, uint32_t max_block = 256) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_plugin_params_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = max_block;
        cfg.playback.max_delay_frames = 1024;
        cfg.playback.plugin_param_capacity = kCapacity;
        cfg.playback.command_capacity = 4;
        session = std::make_unique<Session>(cfg);
        in.assign(tracks, std::vector<float>(1024, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 2; ++o) {
            out[o].assign(1024, 0.0f);
            out_ptrs[o] = out[o].data();
        }
    }

    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, DelayLinePool& pool) {
        return playback_test::chain_plan(*session, chains, pool);
    }

    bool publish(PlaybackPlan* p) { return playback_test::publish_plan(*session, p); }

    uint64_t stamp = 1'000'000'000;

    // `callback_ns` as the backend stamps it; by default one per period.
    void block(uint32_t n = kBlock) { block_at(n, stamp += 1'333'333); }
    void block_at(uint32_t n, uint64_t callback_ns) {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        b.callback_ns = callback_ns;
        t_counting = true;
        session->process(b);
        t_counting = false;
    }
};

ChainBinding bound(uint32_t slot, uint32_t store) {
    ChainBinding c;
    c.active = true;
    c.chain_id = 100 + slot;
    c.channel_slot = slot;
    c.param_store = store;
    c.node_state = slot;
    return c;
}

bool same(const ChainParamEvent& e, uint32_t instance, uint32_t param, float value) {
    return e.instance == instance && e.param == param && e.offset == 0 && e.value == value;
}

int test_routed() {
    Rig rig(3);
    RecordingRunner runner;
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());
    // Track 0 on store 5, track 1 dry, track 2 on store 9.
    CHECK(rig.publish(rig.plan({bound(0, 5), ChainBinding{}, bound(1, 9)}, pool)));
    rig.block();
    CHECK(runner.count == 0);

    CHECK(rig.session->post_plugin_param(5, 0, 3, 0.25f));
    CHECK(rig.session->post_plugin_param(9, 1, 0, -6.0f));
    CHECK(rig.session->post_plugin_param(5, 0, 3, 0.5f));
    CHECK(rig.session->post_plugin_param(5, 2, sonora::plugin::kBypassParam, 1.0f));
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    // In plan order: store 5's node first, its three in the order posted;
    // then store 9's one.
    CHECK(runner.count == 4);
    CHECK(runner.seen[0].store == 5 && same(runner.seen[0].event, 0, 3, 0.25f));
    CHECK(runner.seen[1].store == 5 && same(runner.seen[1].event, 0, 3, 0.5f));
    CHECK(runner.seen[2].store == 5 && same(runner.seen[2].event, 2, sonora::plugin::kBypassParam, 1.0f));
    CHECK(runner.seen[3].store == 9 && same(runner.seen[3].event, 1, 0, -6.0f));
    for (std::size_t i = 0; i < runner.count; ++i) CHECK(runner.seen[i].callback == 2);

    // Handed once: the next callback has none.
    rig.block();
    CHECK(runner.count == 4);
    CHECK(rig.session->plugin_params_dropped() == 0);
    CHECK(runner.bad_calls == 0);
    return 0;
}

int test_unrouted() {
    Rig rig(2, kBlock);
    RecordingRunner runner;
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());

    // A plan with no chain: nothing to hand them to.
    CHECK(rig.publish(rig.plan({ChainBinding{}, ChainBinding{}}, pool)));
    CHECK(rig.session->post_plugin_param(5, 0, 0, 1.0f));
    rig.block();
    CHECK(rig.session->plugin_params_dropped() == 1);

    // A store the plan does not name, and a chain bound to no slot (it
    // passes through; the runner never sees it).
    ChainBinding unbound;
    unbound.active = true;
    unbound.param_store = 6;
    CHECK(rig.publish(rig.plan({bound(0, 5), unbound}, pool)));
    CHECK(rig.session->post_plugin_param(7, 0, 0, 1.0f));
    CHECK(rig.session->post_plugin_param(6, 0, 0, 1.0f));
    CHECK(rig.session->post_plugin_param(5, 0, 0, 2.0f));
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(runner.count == 1 && runner.seen[0].store == 5 && same(runner.seen[0].event, 0, 0, 2.0f));
    CHECK(rig.session->plugin_params_dropped() == 3);

    // A callback mixed in pieces runs no chain (insert_chain_runner.h), so
    // its events are dropped too, not kept for a later callback.
    CHECK(rig.session->post_plugin_param(5, 0, 0, 3.0f));
    rig.block(2 * kBlock);
    CHECK(rig.session->chain_bypassed_blocks() == 1);
    CHECK(rig.session->plugin_params_dropped() == 4);
    rig.block();
    CHECK(runner.count == 1);
    return 0;
}

int test_refused() {
    Rig rig(1);
    RecordingRunner runner;
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({bound(0, 5)}, pool)));

    CHECK(!rig.session->post_plugin_param(sonora::kNoParamStore, 0, 0, 1.0f));
    CHECK(!rig.session->post_plugin_param(5, 0, 0, std::nanf("")));
    CHECK(!rig.session->post_plugin_param(5, 0, 0, INFINITY));
    // A ring of kCapacity holds kCapacity - 1.
    uint32_t taken = 0;
    while (rig.session->post_plugin_param(5, 0, taken, float(taken))) ++taken;
    CHECK(taken == kCapacity - 1);
    rig.block();
    CHECK(runner.count == taken);
    for (uint32_t i = 0; i < taken; ++i) CHECK(same(runner.seen[i].event, 0, i, float(i)));
    // Drained: it takes more.
    CHECK(rig.session->post_plugin_param(5, 0, 0, 1.0f));
    CHECK(rig.session->plugin_params_dropped() == 0);
    return 0;
}

int test_locate_apart() {
    Rig rig(1);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({bound(0, 5)}, pool)));
    while (rig.session->post_plugin_param(5, 0, 0, 1.0f)) {
    }
    CHECK(rig.session->post_locate(4800));
    rig.block();
    CHECK(rig.session->transport().playhead() == 4800);
    return 0;
}

// PR #59 review (b): the callback reads no clock. The runner's now_ns is the
// backend's callback_ns; a period the backend did not stamp carries the
// last stamp forward by the last period (64 frames at 48 kHz: 1 333 333 ns).
int test_callback_ns_from_the_backend() {
    Rig rig(1);
    RecordingRunner runner;
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({bound(0, 5)}, pool)));
    rig.block_at(kBlock, 7'000'000'000);
    CHECK(runner.began_at == 7'000'000'000);
    rig.block_at(kBlock, 7'001'000'000);
    CHECK(runner.began_at == 7'001'000'000);
    rig.block_at(kBlock, 0);
    CHECK(runner.began_at == 7'001'000'000 + 1'333'333);
    rig.block_at(kBlock, 0);
    CHECK(runner.began_at == 7'001'000'000 + 2 * 1'333'333);
    rig.block_at(kBlock, 9'000'000'000);
    CHECK(runner.began_at == 9'000'000'000);
    return 0;
}

// PR #59 review (c): a runner with a fixed block runs only callbacks of that
// size (the client requires it of every block). Another period passes the
// chains through, counted, and its parameter changes are dropped, counted:
// the runner is never begun for it.
int test_runner_block_size_gates_the_callback() {
    Rig rig(1);
    RecordingRunner runner(kBlock);
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({bound(0, 5)}, pool)));
    rig.block();
    CHECK(runner.callback == 1);
    CHECK(rig.session->post_plugin_param(5, 0, 0, 1.0f));
    reset_heap_counts();
    rig.block(kBlock / 2);
    CHECK(heap_untouched());
    CHECK(runner.callback == 1);
    CHECK(rig.session->chain_bypassed_blocks() == 1);
    CHECK(rig.session->plugin_params_dropped() == 1);
    rig.block();
    CHECK(runner.callback == 2 && runner.count == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_callback_ns_from_the_backend()) return 1;
    if (test_runner_block_size_gates_the_callback()) return 1;
    if (test_routed()) return 1;
    if (test_unrouted()) return 1;
    if (test_refused()) return 1;
    if (test_locate_apart()) return 1;
    std::printf("plugin_params: ok\n");
    return 0;
}
