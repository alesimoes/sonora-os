// SPDX-License-Identifier: GPL-3.0-or-later
//
// The state block under a writer (ADR-0007 §4.1, 2026-10-12 update item 3):
// a snapshot is never torn, a starved or blocked read says "stale" and
// returns instead of spinning, a paced writer at the fastest rate measured
// clean does not starve a 60 Hz reader, peak cursors belong to each reader,
// and a block of another layout is refused. Run under TSan in CI.
#include "sonora/api/state_view.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/state_block.h"

using namespace sonora::api;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

constexpr uint32_t kTracks = 64;

// Every field of publish k carries k, so a snapshot mixing two publishes
// shows as fields that disagree.
struct Publish {
    TransportFields t;
    std::vector<TrackFields> tracks;
    std::vector<float> peaks;

    explicit Publish(uint32_t n) : tracks(n), peaks(n, 0.0f) {}

    void fill(uint32_t k) {
        t.state = static_cast<TransportState>(k % 3);
        t.take = static_cast<int32_t>(k);
        t.playhead = uint64_t{k} * 1000;
        t.xruns = k;
        t.overruns = k;
        t.clock_steps = k;
        t.health_flags = k;
        t.inputs_connected = k;
        t.buffer_frames = k;
        for (auto& tr : tracks) {
            tr.flags = k;
            tr.gain = static_cast<float>(k);
            tr.overruns = k;
            tr.clip_count = k;
            tr.latency_frames = k;
        }
    }
};

bool consistent(const TransportSnapshot& s) {
    const auto k = static_cast<uint32_t>(s.take);
    return static_cast<uint32_t>(s.state) == k % 3 && s.playhead == uint64_t{k} * 1000 &&
           s.xruns == k && s.overruns == k && s.clock_steps == k && s.health_flags == k &&
           s.inputs_connected == k && s.buffer_frames == k;
}

bool consistent(const TrackSnapshot& s) {
    return s.gain == static_cast<float>(s.flags) && s.overruns == s.flags &&
           s.clip_count == s.flags && s.latency_frames == s.flags;
}

void attach_refusals() {
    StateBlockMemory mem(4);
    StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), 4, 2, 10'000'000));
    StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    CHECK(v.track_count() == 2);
    CHECK(!v.attach(mem.data(), mem.bytes() - 1, error));
    CHECK(error.find("size") != std::string::npos);
    CHECK(!v.attached());
    CHECK(!v.attach(nullptr, mem.bytes(), error));

    auto* h = static_cast<StateHeader*>(mem.data());
    h->layout_version = kStateLayoutVersion + 1;
    CHECK(!v.attach(mem.data(), mem.bytes(), error));
    CHECK(error.find("layout version " + std::to_string(kStateLayoutVersion + 1)) !=
          std::string::npos);
    h->layout_version = kStateLayoutVersion;
    h->magic = 0;
    CHECK(!v.attach(mem.data(), mem.bytes(), error));
    CHECK(error.find("not an engine state block") != std::string::npos);

    // Misaligned or short memory is refused by the writer too.
    StateWriter w2;
    CHECK(!w2.init(static_cast<unsigned char*>(mem.data()) + 8, mem.bytes() - 8, 2, 2, 1));
    CHECK(!w2.init(mem.data(), mem.bytes(), 4, 5, 1));
    // A detached view reads stale, never garbage.
    StateView none;
    CHECK(none.transport().stale);
    CHECK(none.take_peak(0) == 0.0f);
    CHECK(!none.engine_alive(0));
}

// A writer that stops inside its section: the reader gives up after its
// bounded retries and says stale, in bounded time, rather than spin.
void blocked_writer_reads_stale() {
    StateBlockMemory mem(kTracks);
    StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), kTracks, kTracks, 1'000'000));
    Publish p(kTracks);
    p.fill(5);
    w.publish(p.t, p.tracks.data(), p.peaks.data());
    StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    CHECK(!v.transport().stale);
    CHECK(v.transport().take == 5);

    w.begin_write_for_test();
    const auto t0 = Clock::now();
    const TransportSnapshot s = v.transport();
    const auto first = Clock::now() - t0;
    // A writer that never finishes (one that died mid-section, after M7)
    // costs the first read its retries and every later read nothing, so a
    // tick that reads 64 tracks is not 64 times as slow.
    const auto t1 = Clock::now();
    TrackSnapshot ts;
    for (uint32_t i = 0; i < kTracks; ++i) ts = v.track(i);
    const auto rest = Clock::now() - t1;
    CHECK(s.stale);
    CHECK(s.take == 0 && s.playhead == 0);   // the stale snapshot shows nothing
    CHECK(ts.stale);
    CHECK(v.stale_reads() == 1 + kTracks);
    CHECK(first < std::chrono::milliseconds(50));
    CHECK(rest < std::chrono::milliseconds(5));
    std::printf("  blocked writer: first read stale after %.3f ms, %u more in %.3f ms\n",
                std::chrono::duration<double, std::milli>(first).count(), kTracks,
                std::chrono::duration<double, std::milli>(rest).count());
    w.finish_write_for_test();
    CHECK(!v.transport().stale);
}

// Unpaced: the worst case. The reader may be told stale; it must never be
// handed a torn snapshot.
void never_torn_unpaced() {
    StateBlockMemory mem(kTracks);
    StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), kTracks, kTracks, 0));
    // Before the reader starts: the block as init leaves it (gain 1, all
    // else 0) is valid, but not a publish that consistent() can check.
    Publish first(kTracks);
    first.fill(1);
    w.publish(first.t, first.tracks.data(), first.peaks.data());
    std::atomic<bool> stop{false};
    std::thread writer([&]() {
        Publish p(kTracks);
        for (uint32_t k = 2; !stop.load(std::memory_order_relaxed); ++k) {
            p.fill(k);
            w.publish(p.t, p.tracks.data(), p.peaks.data());
        }
    });
    StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    uint64_t reads = 0, stale = 0, torn = 0;
    const auto end = Clock::now() + std::chrono::milliseconds(400);
    while (Clock::now() < end) {
        const TransportSnapshot s = v.transport();
        ++reads;
        if (s.stale) {
            ++stale;
        } else if (!consistent(s)) {
            ++torn;
        }
        const TrackSnapshot t = v.track(static_cast<uint32_t>(reads % kTracks));
        if (!t.stale && !consistent(t)) ++torn;
    }
    stop = true;
    writer.join();
    CHECK(torn == 0);
    CHECK(reads > 0);
    std::printf("  unpaced writer: %llu reads, %llu stale, %llu torn\n",
                static_cast<unsigned long long>(reads), static_cast<unsigned long long>(stale),
                static_cast<unsigned long long>(torn));
}

// The regression the 2026-10-12 measurement asked for: a writer paced at
// the fastest rate measured clean (1 kHz) does not starve a 60 Hz reader.
// An unpaced republish loop starved one to a p99 of 41 ms.
void paced_writer_does_not_starve_a_60hz_reader() {
    StateBlockMemory mem(kTracks);
    StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), kTracks, kTracks, 1'000'000));
    Publish first(kTracks);
    first.fill(1);
    w.publish(first.t, first.tracks.data(), first.peaks.data());
    std::atomic<bool> stop{false};
    std::thread writer([&]() {
        Publish p(kTracks);
        auto next = Clock::now();
        for (uint32_t k = 2; !stop.load(std::memory_order_relaxed); ++k) {
            p.fill(k);
            for (uint32_t i = 0; i < kTracks; ++i) p.peaks[i] = static_cast<float>(k % 7) / 8;
            w.publish(p.t, p.tracks.data(), p.peaks.data());
            next += std::chrono::microseconds(1000);
            std::this_thread::sleep_until(next);
        }
    });
    StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    int ticks = 0, stale = 0, torn = 0;
    Clock::duration worst{};
    auto next = Clock::now();
    for (; ticks < 60; ++ticks) {
        const auto t0 = Clock::now();
        const TransportSnapshot s = v.transport();
        if (s.stale) ++stale;
        if (!s.stale && !consistent(s)) ++torn;
        for (uint32_t i = 0; i < kTracks; ++i) {
            const TrackSnapshot t = v.track(i);
            if (t.stale) ++stale;
            if (!t.stale && !consistent(t)) ++torn;
            (void)v.take_peak(i);
        }
        worst = std::max(worst, Clock::now() - t0);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    stop = true;
    writer.join();
    CHECK(torn == 0);
    // One frame's worth of reads, 65 of them, 60 times: none stale.
    CHECK(stale == 0);
    // The whole tick's reads within a frame, with margin for a sanitizer.
    CHECK(worst < std::chrono::milliseconds(16));
    std::printf("  paced 1 kHz writer, 60 Hz reader: %d ticks, %d stale, worst tick %.3f ms\n",
                ticks, stale,
                std::chrono::duration<double, std::milli>(worst).count());
}

void publish_peaks(StateWriter& w, uint32_t n, float peak) {
    Publish p(n);
    p.fill(1);
    for (auto& x : p.peaks) x = peak;
    w.publish(p.t, p.tracks.data(), p.peaks.data());
}

void peak_cursors_are_per_reader() {
    StateBlockMemory mem(2);
    StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), 2, 2, 10'000'000));
    StateView a, b;
    std::string error;
    CHECK(a.attach(mem.data(), mem.bytes(), error));
    CHECK(b.attach(mem.data(), mem.bytes(), error));
    CHECK(a.take_peak(0) == 0.0f);   // nothing published yet

    publish_peaks(w, 2, 0.1f);
    publish_peaks(w, 2, 0.5f);
    publish_peaks(w, 2, 0.2f);
    CHECK(a.take_peak(0) == 0.5f);
    // A's read took nothing from B.
    publish_peaks(w, 2, 0.3f);
    CHECK(b.take_peak(0) == 0.5f);
    CHECK(a.take_peak(0) == 0.3f);
    CHECK(a.take_peak(0) == 0.0f);   // nothing new since
    CHECK(b.take_peak(0) == 0.0f);
    // Per track as well as per reader.
    CHECK(a.take_peak(1) == 0.5f);

    // A copy is a second reader at the same position.
    publish_peaks(w, 2, 0.7f);
    StateView c = a;
    CHECK(a.take_peak(0) == 0.7f);
    CHECK(c.take_peak(0) == 0.7f);

    // A reader that attaches late is not shown the past.
    StateView late;
    CHECK(late.attach(mem.data(), mem.bytes(), error));
    CHECK(late.take_peak(0) == 0.0f);

    // NaN never wins; a full-scale over does.
    publish_peaks(w, 2, std::numeric_limits<float>::quiet_NaN());
    publish_peaks(w, 2, 1.25f);
    CHECK(late.take_peak(0) == 1.25f);

    // A reader a whole ring behind gets the newest kPeakSlots, not garbage.
    publish_peaks(w, 2, 0.9f);
    for (uint32_t i = 0; i < kPeakSlots; ++i) publish_peaks(w, 2, 0.25f);
    CHECK(late.take_peak(0) == 0.25f);
    // Out of range is silence, not a read past the block.
    CHECK(late.take_peak(2) == 0.0f);
    CHECK(late.track(2).stale);
}

void heartbeat() {
    StateBlockMemory mem(1);
    StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), 1, 1, 10'000'000));
    StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    const uint64_t s = 1'000'000'000;
    const uint64_t timeout = StateView::kMinAliveTimeoutNs;
    CHECK(!v.engine_alive(s));                  // never published
    publish_peaks(w, 1, 0.0f);
    CHECK(v.engine_alive(s + 1));
    CHECK(v.engine_alive(s + timeout / 2));     // still within the timeout
    CHECK(!v.engine_alive(s + 1 + timeout));    // stood still too long
    publish_peaks(w, 1, 0.0f);
    CHECK(v.engine_alive(s + 2 * timeout));     // beating again
    // A slow publisher gets a longer timeout: ten of its periods.
    StateBlockMemory slow_mem(1);
    StateWriter slow;
    CHECK(slow.init(slow_mem.data(), slow_mem.bytes(), 1, 1, 100'000'000));
    StateView sv;
    CHECK(sv.attach(slow_mem.data(), slow_mem.bytes(), error));
    publish_peaks(slow, 1, 0.0f);
    CHECK(sv.engine_alive(s));
    CHECK(sv.engine_alive(s + 900'000'000));
    CHECK(!sv.engine_alive(s + 1'000'000'000));
}

}  // namespace

int main() {
    attach_refusals();
    blocked_writer_reads_stale();
    never_torn_unpaced();
    paced_writer_does_not_starve_a_60hz_reader();
    peak_cursors_are_per_reader();
    heartbeat();
    if (failures) return EXIT_FAILURE;
    std::printf("api state view: ok\n");
    return EXIT_SUCCESS;
}
