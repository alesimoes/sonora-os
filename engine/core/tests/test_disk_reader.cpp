// SPDX-License-Identifier: GPL-3.0-or-later
//
// The disk reader and its chunk cache (ADR-0009 §4.3, §4.4): residency of a
// plan's window, coalesced reads, the pool never evicting what the playing
// plan needs ahead, buffers reused only once a block has passed, everything
// reusable after deactivate(); and, under TSan, a reader linking and
// unlinking chunks while plans are swapped and a callback reads them.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "playback_test.h"
#include "sonora/disk_reader.h"

namespace {

using namespace playback_test;
using sonora::DiskReader;
using sonora::kNoChunk;
using sonora::PlanExchange;
using sonora::PlaybackPlan;
using sonora::ReaderConfig;
using sonora::Transport;

constexpr uint32_t kChunk = 256;

ReaderConfig config(uint32_t pool, int64_t window) {
    ReaderConfig c;
    c.chunk_frames = kChunk;
    c.window_frames = window;
    c.pool_chunks = pool;
    c.max_sources = 8;
    c.max_coalesce = 8;
    c.wake_ms = 1;
    return c;
}

// Whether every linked slot of `plan` holds the source data it names.
bool slots_hold_their_chunks(const DiskReader& r, const PlaybackPlan& plan, uint32_t source_id) {
    for (uint32_t si = 0; si < plan.segment_count; ++si) {
        const sonora::PlanSegment& s = plan.segments[si];
        for (uint32_t c = 0; c < s.channels; ++c) {
            for (uint32_t k = 0; k < s.chunk_count[c]; ++k) {
                const uint32_t b = sonora::load_chunk_slot(plan, s.first_chunk_slot[c] + k);
                if (b == kNoChunk) continue;
                const float* d = r.chunk_data(b);
                const int64_t base = static_cast<int64_t>(s.first_chunk_index[c] + k) * kChunk;
                for (uint32_t f = 0; f < kChunk; f += 17) {
                    if (d[f] != source_value(source_id, s.source_channel[c], base + f)) return false;
                }
            }
        }
    }
    return true;
}

uint32_t linked_slots(const PlaybackPlan& plan) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < plan.chunk_slot_count; ++i) {
        if (plan.chunk_slots[i].load() != kNoChunk) ++n;
    }
    return n;
}

int test_window_becomes_resident() {
    const std::string path = "test_disk_reader_a.wav";
    CHECK(write_source(path, 3, 2, 40 * kChunk));
    PlanExchange x;
    Transport t(48000);
    DiskReader r(config(64, 8 * kChunk), x, t);
    uint32_t slot = 99;
    std::string error;
    CHECK(r.add_source(path, slot, error));
    CHECK(slot == 0);
    CHECK(r.source_channels(0) == 2);
    CHECK(r.source_frames(0) == 40 * kChunk);

    // A stereo region at timeline 1000 playing source frames from 100.
    SegmentSpec s;
    s.start = 1000;
    s.end = 1000 + 30 * kChunk;
    s.source_offset = 100;
    s.channels = 2;
    PlaybackPlan* p = make_plan(x.next_epoch(), 1, {s}, kChunk);

    const uint64_t gen = r.serve(nullptr, p);
    CHECK(r.resident() != gen);
    CHECK(r.passes() == 0);
    r.run_pass();
    CHECK(r.passes() == 1);
    CHECK(r.adopted() == gen);
    CHECK(r.resident() == gen);
    // [0, 8 chunks) of timeline from playhead 0 needs source frames
    // [100, 100 + 8*256 - 1000): chunks 0..4, both channels. No more.
    CHECK(linked_slots(*p) == 2 * 5);
    CHECK(slots_hold_their_chunks(r, *p, 3));
    // Coalesced: one read for the five chunks.
    CHECK(r.reads() == 1);
    CHECK(r.chunks_read() == 5);

    // A plan pending with the same source shares its chunks: nothing read.
    PlaybackPlan* q = make_plan(x.next_epoch(), 1, {s}, kChunk);
    const uint64_t gen2 = r.serve(p, q);
    r.run_pass();
    CHECK(r.resident() == gen2);
    CHECK(r.chunks_read() == 5);
    CHECK(linked_slots(*q) == 2 * 5);

    r.serve(nullptr, nullptr);
    r.run_pass();
    sonora::destroy_plan(p);
    sonora::destroy_plan(q);
    std::remove(path.c_str());
    return 0;
}

int test_pool_too_small_stays_pending() {
    const std::string path = "test_disk_reader_b.wav";
    CHECK(write_source(path, 1, 1, 40 * kChunk));
    PlanExchange x;
    Transport t(48000);
    // Eight chunks of window, four buffers: never resident, counted.
    DiskReader r(config(4, 8 * kChunk), x, t);
    uint32_t slot = 0;
    std::string error;
    CHECK(r.add_source(path, slot, error));
    SegmentSpec s;
    s.end = 30 * kChunk;
    PlaybackPlan* p = make_plan(x.next_epoch(), 1, {s}, kChunk);
    const uint64_t gen = r.serve(nullptr, p);
    r.run_pass();
    r.run_pass();
    CHECK(r.resident() != gen);
    CHECK(r.cache_full() > 0);
    CHECK(slots_hold_their_chunks(r, *p, 1));
    r.serve(nullptr, nullptr);
    r.run_pass();
    sonora::destroy_plan(p);
    std::remove(path.c_str());
    return 0;
}

// The playing plan rolls past its chunks. Behind the playhead they are
// evicted for what is ahead, but a buffer is reused only once a block has
// passed since its unlink; and never one in [playhead, playhead + W).
int test_eviction_waits_for_a_block() {
    const std::string path = "test_disk_reader_c.wav";
    CHECK(write_source(path, 2, 1, 64 * kChunk));
    PlanExchange x;
    Transport t(48000);
    DiskReader r(config(6, 4 * kChunk), x, t);
    uint32_t slot = 0;
    std::string error;
    CHECK(r.add_source(path, slot, error));
    SegmentSpec s;
    s.end = 60 * kChunk;
    PlaybackPlan* p = make_plan(x.next_epoch(), 1, {s}, kChunk);
    CHECK(x.publish(p));
    const uint64_t gen = r.serve(p, nullptr);
    r.run_pass();
    CHECK(r.resident() == gen);
    CHECK(linked_slots(*p) == 4);

    // Ten chunks on. The four behind are unlinked in this pass; none of
    // them may be reused before a block has run.
    t.set_playhead(10 * kChunk);
    r.run_pass();
    for (uint32_t k = 0; k < 4; ++k) CHECK(p->chunk_slots[k].load() == kNoChunk);
    const uint64_t read_before = r.chunks_read();
    // Two free buffers were left; with no block run nothing else may go.
    CHECK(read_before == 4 + 2);
    r.run_pass();
    CHECK(r.chunks_read() == read_before);
    x.acquire();  // one block
    r.run_pass();
    CHECK(r.chunks_read() == read_before + 2);
    CHECK(slots_hold_their_chunks(r, *p, 2));
    for (uint32_t k = 10; k < 14; ++k) CHECK(p->chunk_slots[k].load() != kNoChunk);

    // Stalled callback: nothing is reused. After deactivate(), all of it is.
    t.set_playhead(30 * kChunk);
    r.run_pass();
    r.run_pass();
    const uint64_t stalled = r.chunks_read();
    r.after_deactivate();
    r.run_pass();
    CHECK(r.chunks_read() > stalled);
    CHECK(slots_hold_their_chunks(r, *p, 2));
    r.serve(nullptr, nullptr);
    r.run_pass();
    std::remove(path.c_str());
    return 0;
}

// A locate in flight protects its window until the transport applies it,
// even behind the playhead.
int test_locate_window() {
    const std::string path = "test_disk_reader_d.wav";
    CHECK(write_source(path, 4, 1, 64 * kChunk));
    PlanExchange x;
    Transport t(48000);
    DiskReader r(config(16, 4 * kChunk), x, t);
    uint32_t slot = 0;
    std::string error;
    CHECK(r.add_source(path, slot, error));
    SegmentSpec s;
    s.end = 60 * kChunk;
    PlaybackPlan* p = make_plan(x.next_epoch(), 1, {s}, kChunk);
    CHECK(x.publish(p));
    t.set_playhead(40 * kChunk);
    r.serve(p, nullptr);
    r.run_pass();
    const uint64_t gen = r.serve(p, nullptr, 2 * kChunk, 1);
    r.run_pass();
    CHECK(r.resident() == gen);
    for (uint32_t k = 2; k < 6; ++k) CHECK(p->chunk_slots[k].load() != kNoChunk);
    for (uint32_t k = 40; k < 44; ++k) CHECK(p->chunk_slots[k].load() != kNoChunk);
    CHECK(slots_hold_their_chunks(r, *p, 4));
    r.serve(nullptr, nullptr);
    r.run_pass();
    std::remove(path.c_str());
    return 0;
}

// A coalesced read covers every channel any chunk of the run needs, so
// some (channel, chunk) pairs it reads are wanted by nobody. Those must not
// hold a buffer that nothing can ever reclaim: once the plan is gone, the
// whole pool serves the next one.
int test_coalesced_read_leaks_nothing() {
    const std::string stereo = "test_disk_reader_f.wav";
    const std::string mono = "test_disk_reader_g.wav";
    CHECK(write_source(stereo, 6, 2, 16 * kChunk));
    CHECK(write_source(mono, 7, 1, 16 * kChunk));
    PlanExchange x;
    Transport t(48000);
    DiskReader r(config(8, 8 * kChunk), x, t);
    uint32_t a = 0;
    uint32_t b = 0;
    std::string error;
    CHECK(r.add_source(stereo, a, error));
    CHECK(r.add_source(mono, b, error));

    // Channel 0 of chunks 0..3 and channel 1 of chunks 2..3: one run of
    // four chunks over both channels, of which six pairs are wanted.
    SegmentSpec left;
    left.end = 4 * kChunk;
    left.source_slot = a;
    SegmentSpec right;
    right.track = 1;
    right.start = 2 * kChunk;
    right.end = 4 * kChunk;
    right.source_offset = 2 * kChunk;
    right.source_slot = a;
    right.source_channel0 = 1;
    PlaybackPlan* p = make_plan(x.next_epoch(), 2, {left, right}, kChunk);
    const uint64_t gen = r.serve(nullptr, p);
    r.run_pass();
    CHECK(r.resident() == gen);
    CHECK(r.reads() == 1);
    CHECK(linked_slots(*p) == 6);

    // Dropped, a block passes, and a plan needing all eight buffers comes.
    r.serve(nullptr, nullptr);
    r.run_pass();
    sonora::destroy_plan(p);
    x.acquire();
    SegmentSpec whole;
    whole.end = 16 * kChunk;
    whole.source_slot = b;
    PlaybackPlan* q = make_plan(x.next_epoch(), 1, {whole}, kChunk);
    const uint64_t gen2 = r.serve(nullptr, q);
    r.run_pass();
    CHECK(r.resident() == gen2);
    CHECK(linked_slots(*q) == 8);
    CHECK(slots_hold_their_chunks(r, *q, 7));
    r.serve(nullptr, nullptr);
    r.run_pass();
    sonora::destroy_plan(q);
    std::remove(stereo.c_str());
    std::remove(mono.c_str());
    return 0;
}

// ADR-0009 §4.1's stress test, reader half: a reader thread links and
// unlinks chunks in a small pool while a publisher swaps plans through the
// residency gate and a callback thread rolls through them, reading every
// slot it finds linked. A buffer reused while a block reads it shows as
// wrong data, or as a race under TSan.
int test_stress_with_reader() {
    const std::string path = "test_disk_reader_e.wav";
    constexpr int64_t kFrames = 200 * kChunk;
    CHECK(write_source(path, 5, 1, kFrames));
    PlanExchange x;
    Transport t(48000);
    DiskReader r(config(24, 4 * kChunk), x, t);
    uint32_t slot = 0;
    std::string error;
    CHECK(r.add_source(path, slot, error));
    r.start();

    std::atomic<bool> run{false};
    std::atomic<bool> bad{false};
    std::atomic<uint64_t> blocks{0};
    const auto callback = [&] {
        uint64_t n = 0;
        while (run.load(std::memory_order_relaxed)) {
            const PlaybackPlan* p = x.acquire();
            if (p) {
                // Every slot, not only the playhead's: the reader unlinks
                // behind it and links ahead, and both must be safe to see.
                if (!slots_hold_their_chunks(r, *p, 5)) bad.store(true);
                // Some blocks are slow between loading a slot and reading
                // its buffer, across several reader passes: the case the
                // block counter exists for.
                if (++n % 16 == 0) {
                    const sonora::PlanSegment& s = p->segments[0];
                    uint32_t held_buffer[8];
                    int64_t held_base[8];
                    int count = 0;
                    for (uint32_t k = 0; k < s.chunk_count[0] && count < 8; ++k) {
                        const uint32_t b = sonora::load_chunk_slot(*p, s.first_chunk_slot[0] + k);
                        if (b == kNoChunk) continue;
                        held_buffer[count] = b;
                        held_base[count++] = static_cast<int64_t>(s.first_chunk_index[0] + k) * kChunk;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    for (int i = 0; i < count; ++i) {
                        const float* d = r.chunk_data(held_buffer[i]);
                        for (uint32_t f = 0; f < kChunk; f += 31) {
                            if (d[f] != source_value(5, 0, held_base[i] + f)) bad.store(true);
                        }
                    }
                }
            }
            const uint64_t ph = t.playhead() + 64;
            t.set_playhead(ph >= static_cast<uint64_t>(kFrames - 8 * kChunk) ? 0 : ph);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    };
    // AudioBackend::activate() and deactivate(): when deactivate returns, no
    // block runs and none will until the next activate.
    std::thread audio;
    const auto activate = [&] {
        run.store(true);
        audio = std::thread(callback);
    };
    const auto deactivate = [&] {
        run.store(false);
        audio.join();
    };
    activate();

    struct Held {
        PlaybackPlan* plan;
        uint64_t dropped_at;
    };
    std::vector<Held> held;
    PlaybackPlan* playing = nullptr;
    uint64_t swaps = 0;
    int cycles = 0;
    const auto may_free = [](void* ctx, const PlaybackPlan* plan) {
        auto* h = static_cast<std::pair<std::vector<Held>*, DiskReader*>*>(ctx);
        for (const Held& e : *h->first) {
            if (e.plan == plan) return h->second->adopted() >= e.dropped_at;
        }
        return true;
    };
    std::pair<std::vector<Held>*, DiskReader*> ctx{&held, &r};
    const auto forget_freed = [&] {
        held.erase(std::remove_if(held.begin(), held.end(),
                                  [&](const Held& h) { return !x.is_retired(h.plan); }),
                   held.end());
    };
    const auto swap = [&](int i) {
        // Two layouts that read different source offsets, so chunks change.
        SegmentSpec s;
        s.end = kFrames - 8 * kChunk;
        s.source_offset = (i % 2) ? kChunk / 2 : 0;
        PlaybackPlan* next = make_plan(x.next_epoch(), 1, {s}, kChunk);
        const uint64_t gen = r.serve(playing, next);
        while (r.resident() < gen) std::this_thread::yield();
        while (!x.publish(next)) {
            x.reclaim(may_free, &ctx);
            std::this_thread::yield();
        }
        const uint64_t dropped = r.serve(next, nullptr);
        if (playing) held.push_back({playing, dropped});
        playing = next;
        ++swaps;
        x.reclaim(may_free, &ctx);
        forget_freed();
    };
    for (int i = 0; i < 4000; ++i) {
        swap(i);
        // Deactivate and reactivate, as an xrun recovery or a device change
        // does: the aged buffers and the retired plans are freed with no
        // block run, and swaps go on while the callback is gone.
        if (i % 200 == 199) {
            deactivate();
            r.after_deactivate();
            x.reclaim_after_deactivate(may_free, &ctx);
            forget_freed();
            for (int k = 0; k < 3; ++k) swap(i + 1 + k);
            r.after_deactivate();
            x.reclaim_after_deactivate(may_free, &ctx);
            forget_freed();
            activate();
            ++cycles;
        }
    }
    deactivate();
    const uint64_t dropped = r.serve(nullptr, nullptr);
    while (r.adopted() < dropped) std::this_thread::yield();
    r.stop();
    r.after_deactivate();
    CHECK(x.reclaim_after_deactivate() == 0);
    CHECK(!bad.load());
    CHECK(cycles == 20);
    std::printf("reader stress: %llu gated swaps, %d deactivate cycles, %llu blocks, "
                "%llu chunks read, %llu cache-full passes\n",
                static_cast<unsigned long long>(swaps), cycles,
                static_cast<unsigned long long>(blocks.load()),
                static_cast<unsigned long long>(r.chunks_read()),
                static_cast<unsigned long long>(r.cache_full()));
    std::remove(path.c_str());
    return 0;
}

}  // namespace

int main() {
    if (test_window_becomes_resident()) return 1;
    if (test_pool_too_small_stays_pending()) return 1;
    if (test_eviction_waits_for_a_block()) return 1;
    if (test_locate_window()) return 1;
    if (test_coalesced_read_leaks_nothing()) return 1;
    if (test_stress_with_reader()) return 1;
    std::printf("disk_reader: ok\n");
    return 0;
}
