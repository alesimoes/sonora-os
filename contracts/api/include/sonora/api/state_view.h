// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reader's side of the state block (ADR-0007 §4.1, update item 3).
//
// One StateView per reader. Its peak cursors are its own, so the Qt UI, the
// terminal UI and a control surface can meter at once without stealing each
// other's peaks; copying a StateView makes a second, independent reader at
// the same position.
//
// A read never waits on the writer. The seqlock is retried a bounded number
// of times; when the bound is reached the snapshot comes back with `stale`
// set and the interface shows it as such for that tick, rather than spinning
// on engine state on its own UI thread.
//
// Not thread-safe: a reader thread owns its StateView.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/state_block.h"

namespace sonora::api {

struct TransportSnapshot {
    TransportState state = TransportState::Stopped;
    int32_t take = 0;
    uint64_t playhead = 0;
    uint32_t xruns = 0;
    uint32_t overruns = 0;
    uint32_t clock_steps = 0;
    uint32_t health_flags = 0;
    uint32_t inputs_connected = 0;
    uint32_t buffer_frames = 0;
    uint32_t compensation_frames = 0;
    uint32_t compensation_shortfall_frames = 0;
    uint64_t heartbeat = 0;
    // The seqlock ran out of retries: these values may be torn, and are the
    // zeros above. Show "stale", never them.
    bool stale = false;
};

struct InsertSnapshot {
    uint32_t flags = 0;
    uint32_t latency_frames = 0;
    uint32_t misses = 0;
    uint32_t worst_run_us = 0;
    uint32_t nonfinite = 0;

    bool present() const { return (flags & kInsertPresent) != 0; }
    bool faulted() const { return (flags & kInsertFaulted) != 0; }
};

struct TrackSnapshot {
    uint32_t flags = 0;
    float gain = 1.0f;
    uint32_t overruns = 0;
    uint32_t clip_count = 0;
    uint32_t latency_frames = 0;
    std::array<InsertSnapshot, kInsertSlots> inserts{};   // slot A first
    bool stale = false;

    bool armed() const { return (flags & kTrackArmed) != 0; }
    bool muted() const { return (flags & kTrackMuted) != 0; }
    bool input_connected() const { return (flags & kTrackInputConnected) != 0; }
};

// The transport and every track from one publish.
struct StateFrame {
    TransportSnapshot transport;
    std::vector<TrackSnapshot> tracks;   // track_count() entries
};

class StateView {
public:
    // Retries before a read is called stale: a few yields, then short
    // sleeps, about 3 ms in all. The writer's section is a few hundred
    // stores; a reader that collides with it this long is being starved, or
    // the writer is gone, and saying so beats waiting.
    static constexpr int kMaxRetries = 64;
    // The heartbeat may stand still this long, or ten publish periods if
    // that is longer, before the engine counts as gone. Six UI ticks at
    // 60 Hz: long enough not to flicker, short enough that a frozen meter is
    // never on screen for long.
    static constexpr uint64_t kMinAliveTimeoutNs = 250'000'000;

    // Checks magic, layout version and size before reading anything else.
    // A block of another layout is refused, naming both versions.
    bool attach(const void* block, std::size_t bytes, std::string& error);
    bool attached() const { return header_ != nullptr; }

    uint32_t track_count() const;
    uint64_t publish_period_ns() const;

    TransportSnapshot transport() const;
    TrackSnapshot track(uint32_t index) const;
    // The transport and every track in one seqlock read, so what a tick
    // draws is one publish and never a transport from one beside track
    // flags from the next. An interface's tick uses this. `out.tracks` is
    // resized to track_count(), so a frame kept from tick to tick
    // allocates only when the count changes. A stale read marks the
    // transport and every track stale.
    void frame(StateFrame& out) const;

    // Linear peak: the maximum published since this reader's previous call
    // for this track, or since attach. 0 for a track past track_count. A
    // reader that falls more than kPeakSlots publishes behind loses the
    // oldest of them, never the newest.
    float take_peak(uint32_t index);

    // False once the heartbeat has stood still for the timeout, judged on
    // the caller's monotonic clock, so no clock is shared with the engine.
    // The interface then shows meters as "no data", not frozen (ADR-0007 §3).
    bool engine_alive(uint64_t now_ns) const;

    // Seqlock reads that ran out of retries, for diagnostics and the test.
    uint64_t stale_reads() const { return stale_reads_; }

private:
    const StateHeader* header_ = nullptr;
    const TrackState* tracks_ = nullptr;
    std::vector<uint64_t> cursors_;
    mutable uint64_t stale_reads_ = 0;
    mutable uint64_t stuck_seq_ = 0;   // an odd sequence a read gave up on
    mutable uint64_t seen_heartbeat_ = 0;
    mutable uint64_t seen_at_ns_ = 0;
    mutable bool seen_any_ = false;
};

}  // namespace sonora::api
