// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/state_block.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace sonora::api {

namespace {

uint64_t pack_peak(uint64_t tick, float peak) {
    uint32_t bits = 0;
    std::memcpy(&bits, &peak, sizeof bits);
    return (tick & 0xFFFFFFFFull) << 32 | bits;
}

}  // namespace

bool StateWriter::init(void* memory, std::size_t bytes, uint32_t capacity_tracks,
                       uint32_t track_count, uint64_t publish_period_ns) {
    if (memory == nullptr || reinterpret_cast<std::uintptr_t>(memory) % 64 != 0) return false;
    if (bytes < state_block_bytes(capacity_tracks) || track_count > capacity_tracks) {
        return false;
    }
    std::memset(memory, 0, state_block_bytes(capacity_tracks));
    auto* h = new (memory) StateHeader;
    h->magic = kStateMagic;
    h->layout_version = kStateLayoutVersion;
    h->capacity_tracks = capacity_tracks;
    h->peak_slots = kPeakSlots;
    h->publish_period_ns = publish_period_ns;
    h->block_bytes = state_block_bytes(capacity_tracks);
    h->track_count.store(track_count, std::memory_order_relaxed);
    h->heartbeat.store(0, std::memory_order_relaxed);
    h->seq.store(0, std::memory_order_relaxed);
    h->transport_state.store(0, std::memory_order_relaxed);
    h->take.store(0, std::memory_order_relaxed);
    h->playhead.store(0, std::memory_order_relaxed);
    h->xruns.store(0, std::memory_order_relaxed);
    h->overruns.store(0, std::memory_order_relaxed);
    h->clock_steps.store(0, std::memory_order_relaxed);
    h->health_flags.store(0, std::memory_order_relaxed);
    h->inputs_connected.store(0, std::memory_order_relaxed);
    h->buffer_frames.store(0, std::memory_order_relaxed);

    TrackState* t = state_tracks(memory);
    for (uint32_t i = 0; i < capacity_tracks; ++i) {
        auto* ts = new (&t[i]) TrackState;
        ts->flags.store(0, std::memory_order_relaxed);
        ts->gain.store(1.0f, std::memory_order_relaxed);
        ts->overruns.store(0, std::memory_order_relaxed);
        ts->clip_count.store(0, std::memory_order_relaxed);
        ts->latency_frames.store(0, std::memory_order_relaxed);
        for (auto& s : ts->peaks.entries) s.store(0, std::memory_order_relaxed);
    }
    // Everything above happens-before a reader that learns of the block
    // through whatever hands it over (a thread start, a mutex, SCM_RIGHTS).

    header_ = h;
    tracks_ = t;
    track_count_ = track_count;
    tick_ = 0;
    return true;
}

void StateWriter::publish(const TransportFields& transport, const TrackFields* tracks,
                          const float* peaks) {
    if (header_ == nullptr) return;
    StateHeader& h = *header_;
    // The seqlock without standalone fences, which ThreadSanitizer cannot
    // see: every field is stored with release, so a reader that acquires
    // any new value also sees the odd sequence stored before it, and its
    // second look at the sequence fails. On x86-64 a release store is a
    // plain store; it costs nothing over relaxed.
    constexpr auto rel = std::memory_order_release;
    const uint64_t s = h.seq.load(std::memory_order_relaxed);
    h.seq.store(s + 1, std::memory_order_relaxed);

    h.transport_state.store(static_cast<uint32_t>(transport.state), rel);
    h.take.store(transport.take, rel);
    h.playhead.store(transport.playhead, rel);
    h.xruns.store(transport.xruns, rel);
    h.overruns.store(transport.overruns, rel);
    h.clock_steps.store(transport.clock_steps, rel);
    h.health_flags.store(transport.health_flags, rel);
    h.inputs_connected.store(transport.inputs_connected, rel);
    h.buffer_frames.store(transport.buffer_frames, rel);
    for (uint32_t i = 0; i < track_count_; ++i) {
        TrackState& t = tracks_[i];
        t.flags.store(tracks[i].flags, rel);
        t.gain.store(tracks[i].gain, rel);
        t.overruns.store(tracks[i].overruns, rel);
        t.clip_count.store(tracks[i].clip_count, rel);
        t.latency_frames.store(tracks[i].latency_frames, rel);
    }
    h.seq.store(s + 2, rel);

    // The slots before the heartbeat: a reader that sees tick n finds every
    // slot up to n written.
    const uint64_t tick = tick_ + 1;
    for (uint32_t i = 0; i < track_count_; ++i) {
        tracks_[i].peaks.entries[tick % kPeakSlots].store(pack_peak(tick, peaks[i]),
                                                        std::memory_order_release);
    }
    h.heartbeat.store(tick, std::memory_order_release);
    tick_ = tick;
}

void StateWriter::begin_write_for_test() {
    const uint64_t s = header_->seq.load(std::memory_order_relaxed);
    header_->seq.store(s + 1, std::memory_order_release);
}

void StateWriter::finish_write_for_test() {
    const uint64_t s = header_->seq.load(std::memory_order_relaxed);
    header_->seq.store(s + 1, std::memory_order_release);
}

}  // namespace sonora::api
