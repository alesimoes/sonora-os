// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginClient's audio-thread half against a scripted host in this process:
// a real channel region (contracts/plugin layout 1), stepped by hand so that
// every case is exact. tests/test_plugin_client_host.cpp does the same
// against the real sonora-plugin-host process.
//
//   numbering     blocks per channel from 1, with no gaps
//   depth 1       the output is the previous callback's block, bit for bit
//   misses        fade-out of the last sample over 64 frames, then silence;
//                 ramp-in; a late host costs two counted misses
//   removal       8 counted misses remove the channel, culprit from the streak
//   swap (M5)     the switch's silent block fades, ramps, and is not counted
//   H1            a plan naming a reassigned slot reads and writes nothing
//   NaN guard     a non-finite sample makes the block a miss and a fault
//   parameters    tagged with the block submitted; held while the host is
//                 busy, written before the next submit, never early
//   A2.2, A2.5    the per-callback copy budget; stores by index
//   frame count   a callback of another size submits nothing
#include "sonora/plugin_client/plugin_client.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

using namespace sonora;
using namespace sonora::plugin_client;

namespace {

constexpr uint32_t kBlock = 128;
constexpr uint32_t kMaxFrames = 256;

static_assert(!plugin::kHostWakeEnabled, "the wake ships gated off (ADR-0012 §1.4)");

// A channel region and the host's side of it, driven by the test.
class ScriptedHost {
public:
    ScriptedHost(uint32_t channels, std::vector<uint32_t> params) : params_(std::move(params)) {
        map_ = plugin::make_param_map(params_.data(), uint32_t(params_.size()));
        layout_ = plugin::make_layout(channels, kMaxFrames, map_.param_slots);
        bytes_ = plugin::align_up(layout_.region_size, plugin::kCacheLine);
        base_ = std::aligned_alloc(plugin::kCacheLine, bytes_);
        std::memset(base_, 0, bytes_);
        h = plugin::init_header(base_, layout_, 48000, kBlock, uint32_t(params_.size()), params_.data(), 0);
        applied_.assign(map_.param_slots, 0.f);
        values_.applied = applied_.data();
    }
    ~ScriptedHost() { std::free(base_); }
    ScriptedHost(const ScriptedHost&) = delete;
    ScriptedHost& operator=(const ScriptedHost&) = delete;

    ChannelBinding binding(uint32_t latency = kBlock) const { return {base_, layout_, map_, latency}; }

    bool pending() const { return h->submitted_block.load() != h->completed_block.load(); }

    // Processes the submitted block: parameters by the contract's own
    // algorithm, then out = gain * in (+ `poison` at frame 5 when set).
    void complete(float gain = 1.f, float poison = 0.f, bool use_poison = false) {
        const uint64_t block = h->submitted_block.load(std::memory_order_acquire);
        if (block == h->completed_block.load()) return;
        delivered.clear();
        replaced.clear();
        plugin::consume_block_params(
            *h, plugin::value_table(base_, layout_), map_, values_, block, h->submitted_frames.load(),
            [&](uint32_t i, uint32_t p, uint32_t off, float v) {
                delivered.push_back({block, i, p, off, v});
                return true;
            },
            [&](uint32_t i, uint32_t p, float v) {
                replaced.push_back({block, i, p, 0, v});
                return true;
            });
        for (uint32_t c = 0; c < layout_.channels; ++c) {
            const float* in = plugin::input_plane(base_, layout_, c);
            float* out = plugin::output_plane(base_, layout_, c);
            for (uint32_t i = 0; i < kBlock; ++i) out[i] = gain * in[i];
            if (use_poison) out[5] = poison;
        }
        h->completed_block.store(block, std::memory_order_release);
    }

    const float* input(uint32_t c) const { return plugin::input_plane(base_, layout_, c); }
    std::atomic<float>* table() const { return plugin::value_table(base_, layout_); }

    plugin::ChannelHeader* h = nullptr;
    std::vector<plugin::ParamChange> delivered, replaced;

private:
    std::vector<uint32_t> params_;
    plugin::ParamMap map_;
    plugin::Layout layout_;
    std::size_t bytes_ = 0;
    void* base_ = nullptr;
    std::vector<float> applied_;
    plugin::HostValues values_;
};

// One mono node's buffers and a deterministic input per callback.
struct Mono {
    std::vector<float> in = std::vector<float>(kBlock), out = std::vector<float>(kBlock);
    const float* ip[1];
    float* op[1];
    Mono() {
        ip[0] = in.data();
        op[0] = out.data();
    }
    void fill(uint64_t callback) {
        for (uint32_t i = 0; i < kBlock; ++i) in[i] = float(int((callback * kBlock + i) % 1000) - 500) / 1000.f;
    }
    void constant(float v) { std::fill(in.begin(), in.end(), v); }
};

struct Rig {
    ClientConfig cfg;
    PluginClient client;
    explicit Rig(ClientConfig c = {}) : cfg(c), client(fixed(c)) {}
    static ClientConfig fixed(ClientConfig c) {
        c.max_channels = c.max_channels > 8 ? 8 : c.max_channels;
        c.max_stores = c.max_stores > 8 ? 8 : c.max_stores;
        c.max_nodes = c.max_nodes > 8 ? 8 : c.max_nodes;
        return c;
    }
    // A host on a fresh slot, its store and node reset; the node's ref.
    InsertChainRef bind(ScriptedHost& host, uint32_t store, uint32_t node, uint32_t channels,
                        const plugin::ParamMap& map) {
        const uint32_t slot = client.acquire_slot();
        client.reset_store(store, map, nullptr, nullptr);
        client.reset_node(node);
        InsertChainRef r;
        r.channel_slot = slot;
        r.channel_generation = client.assign(slot, host.binding());
        r.param_store = store;
        r.node_state = node;
        r.channels = channels;
        return r;
    }
    Outcome step(const InsertChainRef& r, Mono& m, const ParamEvent* ev = nullptr, uint32_t n = 0,
                 uint32_t frames = kBlock) {
        client.begin_callback(0);
        const Outcome o = client.process_chain(r, m.ip, m.op, frames, ev, n);
        client.end_callback();
        return o;
    }
};

plugin::ParamMap map_of(std::vector<uint32_t> counts) {
    return plugin::make_param_map(counts.data(), uint32_t(counts.size()));
}

bool same(const float* a, const float* b, uint32_t n) { return std::memcmp(a, b, n * sizeof(float)) == 0; }

void blocks_are_numbered_from_one_and_collected_one_block_later() {
    Rig rig;
    ScriptedHost host(1, {0});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map_of({0}));
    Mono m;
    std::vector<float> previous(kBlock);
    for (uint64_t cb = 1; cb <= 20; ++cb) {
        m.fill(cb);
        const Outcome o = rig.step(r, m);
        CHECK(host.h->submitted_block.load() == cb);
        CHECK(host.h->submitted_frames.load() == kBlock);
        CHECK(same(host.input(0), m.in.data(), kBlock));
        if (cb == 1) {
            // Nothing was submitted before: silence, and not the host's miss.
            CHECK(o == Outcome::Missed);
            for (float x : m.out) CHECK(x == 0.f);
        } else if (cb == 2) {
            // The first block played comes in from 0 (ADR-0012 §3).
            CHECK(o == Outcome::Collected);
            for (uint32_t i = 0; i < kBlock; ++i) CHECK(m.out[i] == previous[i] * (float(i) / float(kBlock)));
        } else {
            CHECK(o == Outcome::Collected);
            CHECK(same(m.out.data(), previous.data(), kBlock));
        }
        previous = m.in;
        host.complete();
    }
    const SlotStatus st = rig.client.slot_status(r.channel_slot);
    CHECK(st.submitted == 20 && st.collected == 19 && st.misses == 0 && st.streak == 0);
    CHECK(st.latency_frames == kBlock && rig.client.latency_frames(r.channel_slot) == kBlock);
}

void a_miss_fades_the_last_sample_out_and_a_late_host_costs_two() {
    Rig rig;
    ScriptedHost host(1, {0});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map_of({0}));
    Mono m;
    m.constant(0.5f);
    for (int i = 0; i < 4; ++i) {
        rig.step(r, m);
        host.complete();
    }
    CHECK(m.out[kBlock - 1] == 0.5f);
    // The host does not finish this callback's block in time.
    CHECK(rig.step(r, m) == Outcome::Collected);
    CHECK(rig.step(r, m) == Outcome::Missed);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const uint32_t k = i + 1;
        const float want = k < kFadeFrames ? 0.5f * (float(kFadeFrames - k) / float(kFadeFrames)) : 0.f;
        CHECK(m.out[i] == want);
    }
    CHECK(m.out[0] == 0.5f * 63.f / 64.f && m.out[kFadeFrames - 1] == 0.f);
    SlotStatus st = rig.client.slot_status(r.channel_slot);
    CHECK(st.misses == 1 && st.streak == 1 && st.busy_skips == 1);
    const uint64_t submitted = host.h->submitted_block.load();
    // It finishes late: that block is discarded, never played a block late.
    host.complete();
    CHECK(rig.step(r, m) == Outcome::Missed);
    for (float x : m.out) CHECK(x == 0.f);
    CHECK(host.h->submitted_block.load() == submitted + 1);
    st = rig.client.slot_status(r.channel_slot);
    CHECK(st.misses == 2 && st.streak == 2);
    host.complete();
    // Recovery ramps in from 0, and the streak ends.
    CHECK(rig.step(r, m) == Outcome::Collected);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(m.out[i] == 0.5f * (float(i) / float(kBlock)));
    CHECK(rig.client.slot_status(r.channel_slot).streak == 0);
}

void eight_misses_remove_the_channel_and_name_the_culprit() {
    Rig rig;
    ScriptedHost host(1, {1, 1, 1});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map_of({1, 1, 1}));
    Mono m;
    rig.step(r, m);
    host.complete();
    rig.step(r, m);
    // Instance 2 hangs; the host says so through current_instance, a word
    // a plugin could also have scribbled on: 99 must be ignored, not used.
    host.h->current_instance.store(2);
    Outcome o = Outcome::Collected;
    int callbacks = 0;
    while (o != Outcome::Removed && callbacks < 20) {
        if (callbacks == 3) host.h->current_instance.store(99);
        if (callbacks == 4) host.h->current_instance.store(2);
        o = rig.step(r, m);
        ++callbacks;
    }
    CHECK(o == Outcome::Removed);
    CHECK(callbacks == int(kMaxConsecutiveMisses));
    SlotStatus st = rig.client.slot_status(r.channel_slot);
    CHECK(st.removed && rig.client.removed(r.channel_slot));
    CHECK(st.culprit == 2);
    CHECK(st.instance_misses[2] == kMaxConsecutiveMisses - 1);
    // Removed: silence, nothing more submitted even once the host is idle.
    host.complete();
    const uint64_t submitted = host.h->submitted_block.load();
    for (int i = 0; i < 5; ++i) {
        CHECK(rig.step(r, m) == Outcome::Removed);
        for (float x : m.out) CHECK(x == 0.f);
    }
    CHECK(host.h->submitted_block.load() == submitted);
}

void a_swap_fades_out_ramps_in_and_is_not_a_miss() {
    Rig rig;
    ScriptedHost old_host(1, {0}), new_host(1, {0});
    const plugin::ParamMap map = map_of({0});
    const InsertChainRef old_ref = rig.bind(old_host, 0, 0, 1, map);
    Mono m;
    m.constant(0.25f);
    for (int i = 0; i < 3; ++i) {
        rig.step(old_ref, m);
        old_host.complete();
    }
    // Make-before-break: the new host is Ready, the plan moves the same
    // node (same node state, same store) to the new slot.
    InsertChainRef new_ref = old_ref;
    new_ref.channel_slot = rig.client.acquire_slot();
    new_ref.channel_generation = rig.client.assign(new_ref.channel_slot, new_host.binding());
    CHECK(rig.step(new_ref, m) == Outcome::Missed);
    // Not a hard cut: the node's fade state carried over the swap (M5).
    CHECK(m.out[0] == 0.25f * 63.f / 64.f);
    SlotStatus st = rig.client.slot_status(new_ref.channel_slot);
    CHECK(st.misses == 0 && st.streak == 0 && st.submitted == 1);
    CHECK(new_host.h->submitted_block.load() == 1);
    new_host.complete();
    CHECK(rig.step(new_ref, m) == Outcome::Collected);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(m.out[i] == 0.25f * (float(i) / float(kBlock)));
    // The old channel's in-flight block was discarded, its slot untouched.
    CHECK(rig.client.slot_status(old_ref.channel_slot).misses == 0);
}

void a_plan_naming_a_reassigned_slot_reads_and_writes_nothing() {
    Rig rig;
    ScriptedHost first(1, {1}), second(1, {1});
    const plugin::ParamMap map = map_of({1});
    const InsertChainRef stale = rig.bind(first, 0, 0, 1, map);
    Mono m;
    rig.step(stale, m);
    first.complete();
    // The slot is retired and given to another host. A retired plan that
    // still names it (the bug the epoch rule exists to prevent) must see a
    // mismatch, not the new host's data.
    rig.client.retire(stale.channel_slot);
    const uint32_t again = rig.client.acquire_slot();
    CHECK(again == stale.channel_slot);
    const uint32_t gen = rig.client.assign(again, second.binding());
    CHECK(gen != stale.channel_generation);
    second.h->completed_block.store(0);
    float* out = plugin::output_plane(const_cast<void*>(static_cast<const void*>(second.h)),
                                      plugin::make_layout(1, kMaxFrames, 1), 0);
    for (uint32_t i = 0; i < kBlock; ++i) out[i] = 9.f;
    m.constant(0.75f);
    const ParamEvent ev{0, 0, 0, 3.f};
    CHECK(rig.step(stale, m, &ev, 1) == Outcome::Missed);
    for (float x : m.out) CHECK(x != 9.f);
    CHECK(second.h->submitted_block.load() == 0);
    CHECK(second.h->param_write.load() == 0 && second.h->values_seq.load() == 0);
    CHECK(second.input(0)[0] == 0.f);
    CHECK(rig.client.node_status(stale.node_state).generation_faults == 1);
    // Generation 0 is "never assigned" and never current.
    InsertChainRef zero = stale;
    zero.channel_generation = 0;
    CHECK(rig.step(zero, m) == Outcome::Missed);
}

void non_finite_output_is_a_miss_and_a_fault() {
    Rig rig;
    ScriptedHost host(1, {0});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map_of({0}));
    Mono m;
    m.constant(0.5f);
    for (int i = 0; i < 3; ++i) {
        rig.step(r, m);
        host.complete();
    }
    rig.step(r, m);
    host.complete(1.f, std::numeric_limits<float>::quiet_NaN(), true);
    CHECK(rig.step(r, m) == Outcome::Missed);
    for (float x : m.out) CHECK(std::isfinite(x));
    CHECK(m.out[0] == 0.5f * 63.f / 64.f);
    host.complete(1.f, std::numeric_limits<float>::infinity(), true);
    CHECK(rig.step(r, m) == Outcome::Missed);
    for (float x : m.out) CHECK(std::isfinite(x));
    const SlotStatus st = rig.client.slot_status(r.channel_slot);
    CHECK(st.nonfinite_samples == 2 && st.nonfinite_blocks == 2);
    CHECK(st.misses == 2 && st.streak == 2);
}

void parameters_are_tagged_held_and_never_early() {
    Rig rig;
    ScriptedHost host(1, {4});
    const plugin::ParamMap map = map_of({4});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map);
    Mono m;
    // First submit: the store is copied in before it (values_seq 1).
    rig.step(r, m);
    CHECK(host.h->values_seq.load() == 1);
    host.complete();
    host.replaced.clear();

    // Sent in a callback that submits: table first, ring entry tagged with
    // the block submitted in the same callback, at its offset.
    const ParamEvent a{0, 2, 40, 0.5f};
    rig.step(r, m, &a, 1);
    CHECK(host.table()[2].load() == 0.5f);
    CHECK(host.h->param_write.load() == 1);
    host.complete();
    CHECK(host.delivered.size() == 1 && host.delivered[0].block == host.h->submitted_block.load() &&
          host.delivered[0].offset == 40 && host.delivered[0].value == 0.5f);
    CHECK(host.replaced.empty());

    // The host is busy: nothing reaches the region, not the table, the
    // ring nor values_seq (rt-engineer M2, auditor L5) ...
    rig.step(r, m);  // submits; the host does not complete it
    const uint32_t seq = host.h->values_seq.load();
    const uint32_t written = host.h->param_write.load();
    const ParamEvent b{0, 1, 7, 0.25f};
    rig.step(r, m, &b, 1);
    CHECK(host.table()[1].load() == 0.f);
    CHECK(host.h->param_write.load() == written && host.h->values_seq.load() == seq);
    CHECK(rig.client.slot_status(r.channel_slot).busy_skips == 1);
    // ... and the next submit copies the store in first: the host gets the
    // value at offset 0 of the next block, late by the skip, never early.
    host.complete();
    host.replaced.clear();
    rig.step(r, m);
    CHECK(host.table()[1].load() == 0.25f);
    CHECK(host.h->values_seq.load() == seq + 1);
    host.complete();
    CHECK(host.replaced.size() == 1 && host.replaced[0].param == 1 && host.replaced[0].value == 0.25f);

    // Bypass rides in the same path.
    const ParamEvent bp{0, plugin::kBypassParam, 0, 1.f};
    rig.step(r, m, &bp, 1);
    CHECK(host.h->bypass[0].load() == 1.f);

    // An offset past the block is clamped to its last frame.
    const ParamEvent late{0, 3, 1000, 0.1f};
    host.complete();
    rig.step(r, m, &late, 1);
    host.complete();
    CHECK(!host.delivered.empty() && host.delivered.back().offset == kBlock - 1);
}

void the_first_submit_copies_wait_for_the_callback_budget() {
    ClientConfig cfg;
    Rig rig(cfg);
    // Three chains of 6000 parameters: 24 KiB of table each, against the
    // 64 KiB a callback may copy (A2.2). The third waits one callback.
    std::vector<ScriptedHost*> hosts;
    std::vector<InsertChainRef> refs;
    ScriptedHost h0(1, {6000}), h1(1, {6000}), h2(1, {6000});
    for (ScriptedHost* h : {&h0, &h1, &h2}) {
        refs.push_back(rig.bind(*h, uint32_t(refs.size()), uint32_t(refs.size()), 1, map_of({6000})));
        hosts.push_back(h);
    }
    CHECK(2 * plugin::value_copy_bytes(plugin::make_layout(1, kMaxFrames, 6000)) <= plugin::kValueCopyBudgetBytes);
    CHECK(3 * plugin::value_copy_bytes(plugin::make_layout(1, kMaxFrames, 6000)) > plugin::kValueCopyBudgetBytes);
    Mono m;
    rig.client.begin_callback(0);
    for (const InsertChainRef& r : refs) rig.client.process_chain(r, m.ip, m.op, kBlock, nullptr, 0);
    rig.client.end_callback();
    CHECK(h0.h->submitted_block.load() == 1 && h1.h->submitted_block.load() == 1);
    CHECK(h2.h->submitted_block.load() == 0 && h2.h->values_seq.load() == 0);
    CHECK(rig.client.slot_status(refs[2].channel_slot).budget_waits == 1);
    for (ScriptedHost* h : hosts) h->complete();
    rig.client.begin_callback(0);
    for (const InsertChainRef& r : refs) rig.client.process_chain(r, m.ip, m.op, kBlock, nullptr, 0);
    rig.client.end_callback();
    CHECK(h2.h->submitted_block.load() == 1 && h2.h->values_seq.load() == 1);
    // The wait was not the host's miss.
    CHECK(rig.client.slot_status(refs[2].channel_slot).misses == 0);
}

void stores_are_addressed_by_the_node_s_index() {
    Rig rig;
    ScriptedHost a(1, {2}), b(1, {2});
    const plugin::ParamMap map = map_of({2});
    const InsertChainRef ra = rig.bind(a, 3, 0, 1, map);
    const InsertChainRef rb = rig.bind(b, 5, 1, 1, map);
    Mono m;
    const ParamEvent ev{0, 1, 0, 0.9f};
    rig.client.begin_callback(0);
    rig.client.process_chain(ra, m.ip, m.op, kBlock, &ev, 1);
    rig.client.process_chain(rb, m.ip, m.op, kBlock, nullptr, 0);
    rig.client.end_callback();
    CHECK(a.table()[1].load() == 0.9f);
    CHECK(b.table()[1].load() == 0.f);
    // A new host for chain A copies store 3 in on its first submit.
    ScriptedHost a2(1, {2});
    InsertChainRef ra2 = ra;
    ra2.channel_slot = rig.client.acquire_slot();
    ra2.channel_generation = rig.client.assign(ra2.channel_slot, a2.binding());
    rig.step(ra2, m);
    CHECK(a2.table()[1].load() == 0.9f);
    // An index past the table is counted, never followed.
    InsertChainRef wild = ra;
    wild.param_store = 1000;
    CHECK(rig.step(wild, m) == Outcome::Missed);
}

void a_callback_of_another_size_submits_nothing() {
    Rig rig;
    ScriptedHost host(1, {1});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map_of({1}));
    Mono m;
    rig.step(r, m);
    host.complete();
    const ParamEvent ev{0, 0, 0, 0.6f};
    CHECK(rig.step(r, m, &ev, 1, 64) == Outcome::Missed);
    CHECK(host.h->submitted_block.load() == 1);
    CHECK(host.table()[0].load() == 0.f);
    CHECK(rig.client.slot_status(r.channel_slot).frame_mismatches == 1);
    // Held: the next submit copies it in.
    rig.step(r, m);
    CHECK(host.table()[0].load() == 0.6f);
}

void the_clock_ticks_only_after_a_submit() {
    Rig rig;
    alignas(64) unsigned char clock_mem[sizeof(plugin::ClockHeader)] = {};
    plugin::ClockHeader* clock = plugin::init_clock(clock_mem, 48000, kBlock, plugin::default_wake_mode());
    CHECK(clock->wake_mode == uint32_t(plugin::WakeMode::Timer));
    rig.client.set_clock(clock);
    rig.client.begin_callback(1234);
    rig.client.end_callback();
    CHECK(clock->callback_ns.load() == 1234 && clock->tick.load() == 0);
    ScriptedHost host(1, {0});
    const InsertChainRef r = rig.bind(host, 0, 0, 1, map_of({0}));
    Mono m;
    rig.client.begin_callback(5678);
    rig.client.process_chain(r, m.ip, m.op, kBlock, nullptr, 0);
    rig.client.end_callback();
    CHECK(clock->callback_ns.load() == 5678 && clock->tick.load() == 1);
}

}  // namespace

int main() {
    blocks_are_numbered_from_one_and_collected_one_block_later();
    a_miss_fades_the_last_sample_out_and_a_late_host_costs_two();
    eight_misses_remove_the_channel_and_name_the_culprit();
    a_swap_fades_out_ramps_in_and_is_not_a_miss();
    a_plan_naming_a_reassigned_slot_reads_and_writes_nothing();
    non_finite_output_is_a_miss_and_a_fault();
    parameters_are_tagged_held_and_never_early();
    the_first_submit_copies_wait_for_the_callback_budget();
    stores_are_addressed_by_the_node_s_index();
    a_callback_of_another_size_submits_nothing();
    the_clock_ticks_only_after_a_submit();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("plugin_client: ok\n");
    return 0;
}
