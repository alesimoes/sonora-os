// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin channel contract, layout version 1 (ADR-0012 §5): every field's
// offset is pinned, offsets agree with the header, a forged or foreign
// header is told apart from an incompatible one, the parameter ring and the
// last-value table follow the block protocol's order (auditor L1, M3), slot
// generations stop a retired plan from touching a reassigned slot (H1), and
// nothing the host writes can move where the engine reads or writes.
#include "sonora/plugin/protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace sonora::plugin;

// ---------------------------------------------------------------------------
// The layout, pinned. Changing any of these numbers is a layout change: move
// kLayoutVersion (or kClockLayoutVersion) in the same commit, and the
// reviewer checks that both moved (ADR-0012 §5.5, ADR-0007 §6). The figures
// are for the LP64 targets the project builds (x86-64, AArch64).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(kLayoutVersion == 1, "the asserts below describe layout version 1");
static_assert(kClockLayoutVersion == 1, "the asserts below describe clock layout version 1");

static_assert(sizeof(ParamChange) == 24, "ParamChange");
static_assert(offsetof(ParamChange, block) == 0, "ParamChange::block");
static_assert(offsetof(ParamChange, instance) == 8, "ParamChange::instance");
static_assert(offsetof(ParamChange, param) == 12, "ParamChange::param");
static_assert(offsetof(ParamChange, offset) == 16, "ParamChange::offset");
static_assert(offsetof(ParamChange, value) == 20, "ParamChange::value");

static_assert(sizeof(InstanceReport) == 32, "InstanceReport");
static_assert(offsetof(InstanceReport, latency_frames) == 0, "InstanceReport::latency_frames");
static_assert(offsetof(InstanceReport, replicas) == 4, "InstanceReport::replicas");
static_assert(offsetof(InstanceReport, last_run_ns) == 8, "InstanceReport::last_run_ns");
static_assert(offsetof(InstanceReport, worst_run_ns) == 16, "InstanceReport::worst_run_ns");
static_assert(offsetof(InstanceReport, nonfinite_samples) == 24, "InstanceReport::nonfinite_samples");

static_assert(sizeof(ChannelHeader) == 7232, "ChannelHeader");
static_assert(alignof(ChannelHeader) == kCacheLine, "ChannelHeader alignment");
static_assert(offsetof(ChannelHeader, magic) == 0, "magic never moves");
static_assert(offsetof(ChannelHeader, layout_version) == 4, "layout_version never moves");
static_assert(offsetof(ChannelHeader, header_bytes) == 8, "header_bytes never moves");
static_assert(offsetof(ChannelHeader, sample_rate) == 12, "sample_rate");
static_assert(offsetof(ChannelHeader, max_frames) == 16, "max_frames");
static_assert(offsetof(ChannelHeader, block_frames) == 20, "block_frames");
static_assert(offsetof(ChannelHeader, channels) == 24, "channels");
static_assert(offsetof(ChannelHeader, pipeline_depth) == 28, "pipeline_depth");
static_assert(offsetof(ChannelHeader, instance_count) == 32, "instance_count");
static_assert(offsetof(ChannelHeader, host_rt_priority) == 36, "host_rt_priority");
static_assert(offsetof(ChannelHeader, param_slots) == 40, "param_slots");
static_assert(offsetof(ChannelHeader, reserved0) == 44, "reserved0");
static_assert(offsetof(ChannelHeader, values_offset) == 48, "values_offset");
static_assert(offsetof(ChannelHeader, input_offset) == 56, "input_offset");
static_assert(offsetof(ChannelHeader, output_offset) == 64, "output_offset");
static_assert(offsetof(ChannelHeader, region_size) == 72, "region_size");
static_assert(offsetof(ChannelHeader, instance_params) == 80, "instance_params");
static_assert(offsetof(ChannelHeader, submitted_block) == 192, "submitted_block");
static_assert(offsetof(ChannelHeader, submitted_frames) == 200, "submitted_frames");
static_assert(offsetof(ChannelHeader, shutdown) == 204, "shutdown");
static_assert(offsetof(ChannelHeader, values_seq) == 208, "values_seq");
static_assert(offsetof(ChannelHeader, reserved1) == 212, "reserved1");
static_assert(offsetof(ChannelHeader, bypass) == 216, "bypass");
static_assert(offsetof(ChannelHeader, param_write) == 320, "param_write");
static_assert(offsetof(ChannelHeader, param_read) == 384, "param_read");
static_assert(offsetof(ChannelHeader, params) == 448, "params");
static_assert(offsetof(ChannelHeader, completed_block) == 6592, "completed_block");
static_assert(offsetof(ChannelHeader, done_word) == 6600, "done_word");
static_assert(offsetof(ChannelHeader, state) == 6604, "state");
// Its own line, off completed_block's (auditor L4 on PR #45).
static_assert(offsetof(ChannelHeader, heartbeat) == 6656, "heartbeat");
static_assert(offsetof(ChannelHeader, latency_frames) == 6664, "latency_frames");
static_assert(offsetof(ChannelHeader, latency_generation) == 6668, "latency_generation");
static_assert(offsetof(ChannelHeader, current_instance) == 6672, "current_instance");
static_assert(offsetof(ChannelHeader, refusal) == 6676, "refusal");
static_assert(offsetof(ChannelHeader, refusal_instance) == 6680, "refusal_instance");
static_assert(offsetof(ChannelHeader, host_flags) == 6684, "host_flags");
static_assert(offsetof(ChannelHeader, worst_block_ns) == 6688, "worst_block_ns");
static_assert(offsetof(ChannelHeader, params_rejected) == 6696, "params_rejected");
static_assert(offsetof(ChannelHeader, params_late) == 6704, "params_late");
static_assert(offsetof(ChannelHeader, instances) == 6712, "instances");

static_assert(sizeof(ClockHeader) == 128, "ClockHeader");
static_assert(offsetof(ClockHeader, magic) == 0, "clock magic never moves");
static_assert(offsetof(ClockHeader, layout_version) == 4, "clock layout_version never moves");
static_assert(offsetof(ClockHeader, header_bytes) == 8, "clock header_bytes never moves");
static_assert(offsetof(ClockHeader, wake_mode) == 12, "wake_mode");
static_assert(offsetof(ClockHeader, sample_rate) == 16, "clock sample_rate");
static_assert(offsetof(ClockHeader, block_frames) == 20, "clock block_frames");
static_assert(offsetof(ClockHeader, period_ns) == 24, "period_ns");
static_assert(offsetof(ClockHeader, tick) == 64, "tick");
static_assert(offsetof(ClockHeader, reserved0) == 68, "clock reserved0");
static_assert(offsetof(ClockHeader, callback_ns) == 72, "callback_ns");
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The gate is off until an amendment records Unverified 1 passing (A1).
static_assert(!kHostWakeEnabled, "the futex wake ships gated off (ADR-0012 §1.4)");
static_assert(kHostExitIncompatible == 3, "exit code 3 means incompatible host (ADR-0012 §5.5)");

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

// A zeroed, cache-line-aligned stand-in for a mapped region.
struct Region {
    std::vector<unsigned char> storage;
    void* base;
    explicit Region(std::size_t size) : storage(size + kCacheLine, 0) {
        const std::size_t pad =
            (kCacheLine - reinterpret_cast<std::uintptr_t>(storage.data()) % kCacheLine) % kCacheLine;
        base = storage.data() + pad;
    }
};

void layout_and_header() {
    const uint32_t ch = 18, frames = 1024;
    const uint32_t counts[3] = {4, 0, 300};
    const ParamMap map = make_param_map(counts, 3);
    CHECK(map.total == 304 && map.param_slots == 304);
    const Layout layout = make_layout(ch, frames, map.param_slots);
    const std::size_t size = layout.region_size;
    Region r(size);
    prefault(r.base, size);
    ChannelHeader* h = init_header(r.base, layout, 48000, 128, 3, counts, 70);
    CHECK(header_valid(*h, size));
    CHECK(!header_valid(*h, size - 1));  // region smaller than it claims
    CHECK(h->header_bytes == sizeof(ChannelHeader));
    CHECK(channel_compatible(version_words(r.base)));
    CHECK(layout.values_offset % kCacheLine == 0 && layout.input_offset % kCacheLine == 0);
    CHECK(layout.input_offset - layout.values_offset >= map.param_slots * sizeof(float));
    CHECK(layout.output_offset - layout.input_offset >= std::size_t(ch) * frames * sizeof(float));
    CHECK(reinterpret_cast<unsigned char*>(output_plane(r.base, layout, ch - 1) + frames) <=
          static_cast<unsigned char*>(r.base) + size);
    CHECK(reinterpret_cast<unsigned char*>(value_table(r.base, layout) + map.param_slots) <=
          reinterpret_cast<unsigned char*>(input_plane(r.base, layout, 0)));
    CHECK(h->current_instance.load() == -1);
    CHECK(layout_of(*h).region_size == layout.region_size);
    const ParamMap seen = param_map_of(*h);
    CHECK(seen.param_slots == map.param_slots && seen.base[2] == 4 && seen.count[2] == 300);

    // A host that scribbles over the fixed part (it is mapped writable)
    // must not move the engine's planes or table: the engine computes them
    // from its own copy of the layout, never from the shared words.
    float* const in_before = input_plane(r.base, layout, 3);
    float* const out_before = output_plane(r.base, layout, 3);
    std::atomic<float>* const table_before = value_table(r.base, layout);
    const uint64_t saved_in = h->input_offset, saved_out = h->output_offset;
    const uint32_t saved_frames = h->max_frames;
    h->input_offset = 0xFFFFFFFFFFFF0000ull;
    h->output_offset = 8;
    h->max_frames = 0x7FFFFFFF;
    h->values_offset = 1;
    CHECK(input_plane(r.base, layout, 3) == in_before);
    CHECK(output_plane(r.base, layout, 3) == out_before);
    CHECK(value_table(r.base, layout) == table_before);
    CHECK(!header_valid(*h, size));  // and a host given such a header refuses it
    h->input_offset = saved_in;
    h->output_offset = saved_out;
    h->max_frames = saved_frames;
    h->values_offset = values_offset();
    CHECK(header_valid(*h, size));

    // Depth 1 only (§1.5).
    h->pipeline_depth = 0;
    CHECK(!header_valid(*h, size));
    h->pipeline_depth = 2;
    CHECK(!header_valid(*h, size));
    h->pipeline_depth = 1;
    // Parameter counts must agree with the table they size.
    h->instance_params[2] = 301;
    CHECK(!header_valid(*h, size));
    h->instance_params[2] = 300;
    h->instance_params[5] = 1;  // past instance_count
    CHECK(!header_valid(*h, size));
    h->instance_params[5] = 0;
    h->param_slots = 303;
    CHECK(!header_valid(*h, size));
    h->param_slots = 304;
    CHECK(header_valid(*h, size));

    // Incompatible versus invalid: a header of another layout version, or
    // of this version with another size, is "not my version" (exit 3); a
    // wrong magic is "not a channel" (exit 2).
    h->layout_version = 0;
    CHECK(!channel_compatible(version_words(r.base)) && !header_valid(*h, size));
    h->layout_version = 2;
    CHECK(!channel_compatible(version_words(r.base)));
    h->layout_version = 1;
    h->header_bytes = 6464;
    CHECK(!channel_compatible(version_words(r.base)) && !header_valid(*h, size));
    h->header_bytes = sizeof(ChannelHeader);
    h->magic ^= 1;
    CHECK(channel_compatible(version_words(r.base)) && !header_valid(*h, size));
    h->magic ^= 1;
    CHECK(header_valid(*h, size));

    // Host-written words are bounds-checked before the engine uses them.
    CHECK(checked_instance(-1, 3) == -1);
    CHECK(checked_instance(2, 3) == 2);
    CHECK(checked_instance(3, 3) == -1);
    CHECK(checked_instance(0x7FFFFFFF, 3) == -1);
    CHECK(checked_instance(-7, 3) == -1);
    CHECK(checked_latency(304) == 304);
    CHECK(checked_latency(kMaxLatencyFrames) == kMaxLatencyFrames);
    CHECK(checked_latency(kMaxLatencyFrames + 1) == kLatencyInvalid);
    CHECK(checked_state(uint32_t(HostState::Ready)) == HostState::Ready);
    CHECK(checked_state(99) == HostState::Faulted);

    // Words written by different processes must not share a cache line.
    const auto line = [&](const void* p) {
        return std::size_t(static_cast<const unsigned char*>(p) - static_cast<unsigned char*>(r.base)) /
               kCacheLine;
    };
    CHECK(line(&h->submitted_block) != line(&h->completed_block));
    CHECK(line(&h->values_seq) != line(&h->completed_block));
    CHECK(line(&h->param_write) != line(&h->param_read));
    // The audio thread reads completed_block every callback: nothing the
    // host writes more than once per block shares its line (L4).
    CHECK(line(&h->heartbeat) != line(&h->completed_block));
    CHECK(line(&h->current_instance) != line(&h->completed_block));
    CHECK(line(&h->worst_block_ns) != line(&h->completed_block));

    // A sample rate of 0 is not a channel.
    h->sample_rate = 0;
    CHECK(!header_valid(*h, size));
    h->sample_rate = 48000;
    CHECK(header_valid(*h, size));
}

void param_map_cap() {
    // Parameters past kMaxParamsPerChain have no slot: not automatable.
    const uint32_t counts[2] = {8000, 500};
    const ParamMap m = make_param_map(counts, 2);
    CHECK(m.total == 8500 && m.param_slots == kMaxParamsPerChain);
    CHECK(param_slot(m, 0, 7999) == 7999);
    CHECK(param_slot(m, 1, 191) == 8191);
    CHECK(param_slot(m, 1, 192) == -1);
    CHECK(param_slot(m, 1, 500) == -1);
    CHECK(param_slot(m, 2, 0) == -1);
    CHECK(param_slot(m, 0, kBypassParam) == -1);
}

struct Applied {
    uint32_t instance, param, offset;
    float value;
    bool replace;
};

struct Fixture {
    uint32_t counts[2] = {3, 2};
    ParamMap map = make_param_map(counts, 2);
    Layout layout = make_layout(1, 256, map.param_slots);
    Region r{layout.region_size};
    ChannelHeader* h = init_header(r.base, layout, 48000, 128, 2, counts, 0);
    std::atomic<float>* table = value_table(r.base, layout);
    EngineChannel e = make_engine_channel(128);
    std::vector<float> applied = std::vector<float>(map.param_slots, 0.f);
    HostValues v;
    std::vector<Applied> log;
    Fixture() { v.applied = applied.data(); }

    uint32_t consume(uint64_t block, uint32_t frames = 128) {
        return consume_block_params(
            *h, table, map, v, block, frames,
            [&](uint32_t i, uint32_t p, uint32_t off, float x) {
                log.push_back({i, p, off, x, false});
                return true;
            },
            [&](uint32_t i, uint32_t p, float x) {
                log.push_back({i, p, 0, x, true});
                return true;
            });
    }
};

void ring_order() {
    Fixture f;
    // Ring: fills to capacity exactly, refuses the next, drains in order.
    for (uint32_t i = 0; i < kParamRingCapacity; ++i) CHECK(push_param(*f.h, f.e, {7, 0, 0, i, float(i)}));
    CHECK(!push_param(*f.h, f.e, {7, 0, 0, 0, 0.f}));
    ParamChange p{};
    for (uint32_t i = 0; i < kParamRingCapacity; ++i) {
        CHECK(pop_param(*f.h, p));
        CHECK(p.offset == i);
    }
    CHECK(!pop_param(*f.h, p));

    // A host that scribbles param_read cannot make the engine write outside
    // the ring: the index is masked, and a read index ahead of the write
    // index reads as a full ring, so pushes are refused and counted.
    f.h->param_read.store(f.h->param_write.load() + 5);
    CHECK(!push_param(*f.h, f.e, {0, 0, 1, 0, 1.f}));
    f.h->param_read.store(f.h->param_write.load());

    // Nor by scribbling param_write: the engine never reads it back (L1).
    // Its own index decides where the entry goes and what is published.
    const uint32_t w = f.e.param_write;
    f.h->param_write.store(w + 100);
    CHECK(push_param(*f.h, f.e, {0, 0, 1, 0, 2.f}));
    CHECK(f.e.param_write == w + 1 && f.h->param_write.load() == w + 1);
    CHECK(f.h->params[w & (kParamRingCapacity - 1)].value == 2.f);
}

void block_tags() {
    Fixture f;
    // Block 10 is being processed: older entries land at offset 0 and are
    // late, entries for 10 at their offset, and the first entry for 11
    // stops the drain and stays.
    CHECK(send_param(*f.h, f.e, f.table, f.map, {9, 0, 1, 77, 0.5f}));
    CHECK(send_param(*f.h, f.e, f.table, f.map, {10, 1, 0, 64, 0.25f}));
    CHECK(send_param(*f.h, f.e, f.table, f.map, {10, 0, 2, 500, 0.75f}));  // offset past frames
    CHECK(send_param(*f.h, f.e, f.table, f.map, {11, 0, 0, 3, 1.f}));
    CHECK(send_param(*f.h, f.e, f.table, f.map, {10, 0, 0, 3, 2.f}));  // behind the 11
    CHECK(f.consume(10) == 3);
    CHECK(f.log.size() == 3);
    CHECK(f.log[0].instance == 0 && f.log[0].param == 1 && f.log[0].offset == 0 && !f.log[0].replace);
    CHECK(f.log[1].instance == 1 && f.log[1].param == 0 && f.log[1].offset == 64);
    CHECK(f.log[2].param == 2 && f.log[2].offset == 127);
    CHECK(f.h->params_late.load() == 1);
    CHECK(f.e.values_seq == 0);  // nothing was refused, so the table was not re-read
    f.log.clear();
    CHECK(f.consume(11) == 2);
    CHECK(f.log.size() == 2 && f.log[0].value == 1.f && f.log[1].value == 2.f && f.log[1].offset == 0);
    CHECK(f.h->params_late.load() == 2);

    // Unknown instance or parameter, and non-finite values: consumed and
    // counted, never applied.
    f.log.clear();
    push_param(*f.h, f.e, {12, 2, 0, 0, 1.f});
    push_param(*f.h, f.e, {12, 1, 2, 0, 1.f});
    push_param(*f.h, f.e, {12, 0, 0, 0, std::numeric_limits<float>::quiet_NaN()});
    push_param(*f.h, f.e, {12, 1, kBypassParam, 0, 1.f});
    CHECK(f.consume(12) == 4);
    CHECK(f.h->params_rejected.load() == 3);
    CHECK(f.log.size() == 1 && f.log[0].param == kBypassParam && f.log[0].instance == 1);
    CHECK(f.v.applied_bypass[1] == 1.f && f.h->bypass[1].load() == 0.f);  // raw push: table untouched
}

void table_precedence() {
    Fixture f;
    // Fill the ring so the next send is refused: the table carries it, and
    // values_seq tells the host to look (M3).
    for (uint32_t i = 0; i < kParamRingCapacity; ++i) CHECK(push_param(*f.h, f.e, {20, 0, 1, 0, 0.1f}));
    CHECK(!send_param(*f.h, f.e, f.table, f.map, {20, 0, 1, 0, 0.9f}));
    CHECK(f.e.values_seq == 1 && f.h->values_seq.load() == 1);
    // L1: the 256 ring entries for slot 1 (0.1) apply first, then the
    // table's newer 0.9 replaces them; the newest wins.
    CHECK(f.consume(20) == kParamRingCapacity);
    CHECK(!f.log.empty() && f.log.back().replace && f.log.back().value == 0.9f &&
          f.log.back().instance == 0 && f.log.back().param == 1);
    CHECK(f.applied[1] == 0.9f && f.v.seen_seq == 1);
    // Nothing changed since: no re-read, nothing applied.
    f.log.clear();
    CHECK(f.consume(21) == 0 && f.log.empty());
    // A bump with no value changed applies nothing.
    bump_values_seq(*f.h, f.e);
    CHECK(f.consume(22) == 0 && f.log.empty());

    // A ring entry that already carried the table's value is not applied
    // twice at offset 0: the table re-read compares with what was applied.
    f.log.clear();
    CHECK(send_param(*f.h, f.e, f.table, f.map, {23, 1, 0, 40, 0.3f}));
    bump_values_seq(*f.h, f.e);
    f.consume(23);
    CHECK(f.log.size() == 1 && !f.log[0].replace && f.log[0].offset == 40);

    // First submit to a new channel (A2): the private store goes in whole,
    // param_slots values and the bypasses, and the host applies what differs.
    f.log.clear();
    const float store[5] = {1.f, 0.9f, 3.f, 0.3f, 5.f};
    const float bypass[2] = {0.f, 1.f};
    CHECK(value_copy_bytes(f.layout) == 5 * sizeof(float) + kMaxInstances * sizeof(float));
    store_values(*f.h, f.e, f.table, f.layout, store, bypass, 2);
    f.consume(24);
    CHECK(f.log.size() == 4);  // slots 0, 2, 4 and instance 1's bypass
    for (const Applied& a : f.log) CHECK(a.replace);
    CHECK(f.applied[0] == 1.f && f.applied[4] == 5.f && f.v.applied_bypass[1] == 1.f);
}

// M3 under concurrency: the engine thread writes the table and bumps
// values_seq while the host thread reads it; once the engine stops and the
// host takes one more block, the host holds every final value. With a
// host-cleared dirty flag the last write could be lost. TSan runs this.
void table_concurrent() {
    Fixture f;
    constexpr int kRounds = 20000;
    std::atomic<bool> done{false};
    std::thread host([&] {
        uint64_t block = 0;
        while (!done.load(std::memory_order_acquire)) {
            f.consume(++block);
            f.log.clear();
        }
        f.consume(++block);
    });
    for (int n = 1; n <= kRounds; ++n) {
        const uint32_t slot = uint32_t(n) % f.map.param_slots;
        seed_value(f.table, slot, float(n));
        bump_values_seq(*f.h, f.e);
    }
    done.store(true, std::memory_order_release);
    host.join();
    for (uint32_t s = 0; s < f.map.param_slots; ++s) {
        int last = 0;
        for (int n = 1; n <= kRounds; ++n)
            if (uint32_t(n) % f.map.param_slots == s) last = n;
        CHECK(f.applied[s] == float(last));
    }
}

// rt-engineer M1 and M3: per-channel numbering from 1, no gaps, and a
// block of any size other than block_frames is never submitted.
void block_numbering() {
    Fixture f;
    CHECK(f.e.block == 0 && next_block(f.e) == 1);
    CHECK(channel_idle(*f.h, f.e));  // a fresh region: nothing outstanding
    CHECK(!submit_block(*f.h, f.e, 64));
    CHECK(!submit_block(*f.h, f.e, 256));
    CHECK(f.h->submitted_block.load() == 0 && f.h->submitted_frames.load() == 0 && f.e.block == 0);
    CHECK(submit_block(*f.h, f.e, 128));
    CHECK(f.h->submitted_block.load() == 1 && f.h->submitted_frames.load() == 128);
    CHECK(!channel_idle(*f.h, f.e));
    f.h->completed_block.store(1);
    CHECK(channel_idle(*f.h, f.e) && next_block(f.e) == 2);
    CHECK(submit_block(*f.h, f.e, 128) && f.h->submitted_block.load() == 2);
    // A host that forges completed_block cannot move the engine's count.
    f.h->completed_block.store(77);
    CHECK(!channel_idle(*f.h, f.e) && next_block(f.e) == 3);
    // A new channel starts again at 1.
    const EngineChannel fresh = make_engine_channel(128);
    CHECK(next_block(fresh) == 1 && fresh.resync && fresh.block_frames == 128);
}

// rt-engineer M2, auditor L5: a change in a callback that does not submit
// is held, never written to the region, so it cannot reach the block the
// host is busy with; it arrives with the next submit, at offset 0.
void held_changes_never_early() {
    Fixture f;
    float store[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
    const float bypass[2] = {0.f, 0.f};
    // Callback 1: the first submit (resync), block 1.
    CHECK(f.e.resync);
    store_values(*f.h, f.e, f.table, f.layout, store, bypass, 2);
    CHECK(!f.e.resync && submit_block(*f.h, f.e, 128));
    // The host starts block 1 and is still busy with it ...
    f.consume(1);
    f.log.clear();
    // ... when callback 2 brings a change: the host is not idle, so it goes
    // to the private store and is held.
    CHECK(!channel_idle(*f.h, f.e));
    const uint32_t seq = f.h->values_seq.load(), w = f.h->param_write.load();
    store[2] = 0.6f;
    hold_param(f.e);
    CHECK(f.e.resync);
    CHECK(f.h->values_seq.load() == seq && f.h->param_write.load() == w && f.table[2].load() == 0.f);
    // Whatever the host does with block 1 now, it cannot see the change.
    f.consume(1);
    CHECK(f.log.empty() && f.applied[2] == 0.f);
    // The host completes block 1; callback 3 submits block 2 with the
    // resync first, and the host applies the value at offset 0 of block 2.
    f.h->completed_block.store(1);
    CHECK(channel_idle(*f.h, f.e));
    store[4] = 0.25f;  // the caller keeps its store current before sending
    store_values(*f.h, f.e, f.table, f.layout, store, bypass, 2);
    CHECK(send_param(*f.h, f.e, f.table, f.map, {next_block(f.e), 1, 1, 9, 0.25f}));
    CHECK(submit_block(*f.h, f.e, 128) && f.h->submitted_block.load() == 2);
    f.consume(2);
    CHECK(f.log.size() == 2);
    CHECK(f.log.size() == 2 && !f.log[0].replace && f.log[0].instance == 1 && f.log[0].param == 1 &&
          f.log[0].offset == 9);
    CHECK(f.log.size() == 2 && f.log[1].replace && f.log[1].instance == 0 && f.log[1].param == 2 &&
          f.log[1].value == 0.6f);
    CHECK(f.h->params_late.load() == 0);
}

// rt-engineer M4: the engine checks its own copy of an output plane.
void collect_guard() {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    float plane[8] = {0.5f, -inf, 1e-30f, nan, inf, -0.f, -nan, 0.25f};
    float out[8];
    CHECK(collect_output(out, plane, 8) == 4);
    const float want[8] = {0.5f, 0.f, 1e-30f, 0.f, 0.f, -0.f, 0.f, 0.25f};
    for (int i = 0; i < 8; ++i) CHECK(same_bits(out[i], want[i]));
    // The plane itself is not the engine's to repair.
    CHECK(!finite_bits(plane[3]));
    float clean[3] = {1.f, 2.f, 3.f};
    CHECK(collect_output(out, clean, 3) == 0 && out[2] == 3.f);
    CHECK(collect_output(out, clean, 0) == 0);
}

// The --version line's grammar is fixed (ADR-0012 §5.5).
void version_line() {
    char buf[96];
    CHECK(format_version_line(buf, sizeof buf) > 0);
    uint32_t layout = 99, clock = 99;
    CHECK(parse_version_line(buf, layout, clock) && layout == kLayoutVersion && clock == kClockLayoutVersion);
    CHECK(parse_version_line("sonora-plugin-host layout 1 clock 1\n", layout, clock));
    CHECK(parse_version_line("sonora-plugin-host layout 7 clock 3 wire 2", layout, clock) && layout == 7 &&
          clock == 3);
    CHECK(parse_version_line("sonora-plugin-host clock 4 layout 5", layout, clock) && layout == 5 && clock == 4);
    CHECK(!parse_version_line("sonora-plugin-host layout 1", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-host layout 1 clock", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-host layout 1 clock x", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-host layout 1 layout 2 clock 1", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-host layout 1  clock 1", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-host layout 1 clock 99999999999", layout, clock));
    CHECK(!parse_version_line("other-host layout 1 clock 1", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-hostess layout 1 clock 1", layout, clock));
    CHECK(!parse_version_line("sonora-plugin-host layout 1 clock 1 trailing", layout, clock));
    CHECK(!parse_version_line(nullptr, layout, clock));
}

void slots() {
    // H1: slot 0 runs host A (generation 1); A dies, its plan is retired,
    // the slot goes to host C (generation 2). A plan still naming
    // generation 1 collects and submits nothing.
    SlotGeneration g;
    CHECK(!slot_current(g, 0));
    const uint32_t a = assign_slot(g);
    CHECK(a == 1 && slot_current(g, a));
    const uint32_t c = assign_slot(g);
    CHECK(c == 2 && slot_current(g, c) && !slot_current(g, a));
    g.value.store(0xFFFFFFFFu);
    CHECK(assign_slot(g) == 1);  // 0 stays "never assigned"
}

void clock_region() {
    Region r(sizeof(ClockHeader));
    ClockHeader* c = init_clock(r.base, 48000, 128, default_wake_mode());
    CHECK(c->wake_mode == uint32_t(WakeMode::Timer));
    CHECK(c->period_ns == 2666666);
    CHECK(clock_valid(*c, sizeof(ClockHeader)));
    CHECK(!clock_valid(*c, sizeof(ClockHeader) - 1));
    CHECK(clock_compatible(version_words(r.base)));
    c->wake_mode = 2;
    CHECK(!clock_valid(*c, sizeof(ClockHeader)));
    c->wake_mode = 1;
    c->period_ns = 1;
    CHECK(!clock_valid(*c, sizeof(ClockHeader)));
    c->period_ns = 2666666;
    c->layout_version = 2;
    CHECK(!clock_compatible(version_words(r.base)) && !clock_valid(*c, sizeof(ClockHeader)));
    c->layout_version = 1;
    CHECK(clock_valid(*c, sizeof(ClockHeader)));

    // The channel and the clock describe the same quantum.
    const uint32_t counts[1] = {1};
    const Layout l = make_layout(1, 256, 1);
    Region cr(l.region_size);
    ChannelHeader* h = init_header(cr.base, l, 48000, 128, 1, counts, 0);
    CHECK(channel_matches_clock(*h, *c));
    h->block_frames = 256;
    CHECK(!channel_matches_clock(*h, *c));
    h->block_frames = 128;
    h->sample_rate = 44100;
    CHECK(!channel_matches_clock(*h, *c));
    c->sample_rate = 0;
    CHECK(!clock_valid(*c, sizeof(ClockHeader)));
    c->sample_rate = 48000;

    publish_callback_start(*c, 1000);
    publish_tick(*c);
    CHECK(c->tick.load() == 1 && c->callback_ns.load() == 1000);

    // The timer path's next look: after this callback's submit if that is
    // still ahead, else the next callback's.
    const uint64_t P = 2666666, off = 150000;
    CHECK(next_wake_ns(1000, P, off, 1000) == 1000 + off);         // in the callback, before its submit
    CHECK(next_wake_ns(1000, P, off, 1000 + off) == 1000 + off + P);
    CHECK(next_wake_ns(1000, P, off, 1000 + 3 * P) == 1000 + off + 3 * P);
    CHECK(next_wake_ns(1000, P, off, 1000 + 3 * P + off) == 1000 + off + 4 * P);
    CHECK(next_wake_ns(1000, 0, off, 5) == 5);
}

}  // namespace

int main() {
    layout_and_header();
    param_map_cap();
    ring_order();
    block_tags();
    table_precedence();
    table_concurrent();
    block_numbering();
    held_changes_never_early();
    collect_guard();
    version_line();
    slots();
    clock_region();
    if (failures) return EXIT_FAILURE;
    std::printf("plugin protocol layout %u: ok (header %zu bytes, clock %zu bytes)\n", kLayoutVersion,
                sizeof(ChannelHeader), sizeof(ClockHeader));
    return EXIT_SUCCESS;
}
