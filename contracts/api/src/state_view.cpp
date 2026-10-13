// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/state_view.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace sonora::api {

namespace {

// The seqlock read: `read` copies the fields with acquire loads, and the
// copy counts only if the sequence was even and unchanged around it (see
// StateWriter::publish for why no fence is needed).
//
// Bounded, and short: a few yields, then short sleeps, so a writer that was
// preempted mid-section gets the core back, about 3 ms at worst before the
// read is called stale. A writer that died mid-section (after M7, another
// process) leaves the sequence odd for good; `stuck` remembers the odd value
// a read gave up on, and a read that finds the same one gives up at once,
// so a dead writer costs one slow read, not one per track per tick.
template <typename Read>
bool seqlock_read(const StateHeader& h, uint64_t& stuck, Read read) {
    uint64_t odd = 0;
    for (int attempt = 0; attempt < StateView::kMaxRetries; ++attempt) {
        if (attempt >= 16) {
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        } else if (attempt > 0) {
            std::this_thread::yield();
        }
        const uint64_t s1 = h.seq.load(std::memory_order_acquire);
        if ((s1 & 1) != 0) {
            if (s1 == stuck) return false;
            odd = s1;
            continue;
        }
        read();
        const uint64_t s2 = h.seq.load(std::memory_order_relaxed);
        if (s1 == s2) {
            stuck = 0;
            return true;
        }
    }
    stuck = odd;
    return false;
}

void read_transport(const StateHeader& h, TransportSnapshot& s) {
    constexpr auto acq = std::memory_order_acquire;
    s.state = static_cast<TransportState>(h.transport_state.load(acq));
    s.take = h.take.load(acq);
    s.playhead = h.playhead.load(acq);
    s.xruns = h.xruns.load(acq);
    s.overruns = h.overruns.load(acq);
    s.clock_steps = h.clock_steps.load(acq);
    s.health_flags = h.health_flags.load(acq);
    s.inputs_connected = h.inputs_connected.load(acq);
    s.buffer_frames = h.buffer_frames.load(acq);
    s.compensation_frames = h.compensation_frames.load(acq);
    s.compensation_shortfall_frames = h.compensation_shortfall_frames.load(acq);
}

void read_track(const TrackState& t, TrackSnapshot& s) {
    constexpr auto acq = std::memory_order_acquire;
    s.flags = t.flags.load(acq);
    s.gain = t.gain.load(acq);
    s.overruns = t.overruns.load(acq);
    s.clip_count = t.clip_count.load(acq);
    s.latency_frames = t.latency_frames.load(acq);
    for (uint32_t k = 0; k < kInsertSlots; ++k) {
        const InsertState& from = t.inserts[k];
        InsertSnapshot& to = s.inserts[k];
        to.flags = from.flags.load(acq);
        to.latency_frames = from.latency_frames.load(acq);
    }
}

// Outside the seqlock read, so they do not lengthen the window a publish
// can collide with; each is one word and needs no other.
void read_insert_counters(const TrackState& t, TrackSnapshot& s) {
    constexpr auto acq = std::memory_order_acquire;
    for (uint32_t k = 0; k < kInsertSlots; ++k) {
        const InsertState& from = t.inserts[k];
        InsertSnapshot& to = s.inserts[k];
        to.misses = from.misses.load(acq);
        to.worst_run_us = from.worst_run_us.load(acq);
        to.nonfinite = from.nonfinite.load(acq);
    }
}

// A value the writer never writes is a torn or foreign block.
bool known_state(TransportState s) {
    return s == TransportState::Stopped || s == TransportState::Rolling ||
           s == TransportState::Recording;
}

}  // namespace

bool StateView::attach(const void* block, std::size_t bytes, std::string& error) {
    header_ = nullptr;
    tracks_ = nullptr;
    if (block == nullptr || bytes < sizeof(StateHeader)) {
        error = "the engine's state block is missing or too small to hold a header";
        return false;
    }
    const auto* h = static_cast<const StateHeader*>(block);
    if (h->magic != kStateMagic) {
        error = "the memory handed over is not an engine state block";
        return false;
    }
    if (h->layout_version != kStateLayoutVersion) {
        error = "the engine's state block is layout version " +
                std::to_string(h->layout_version) + " where this interface reads version " +
                std::to_string(kStateLayoutVersion) +
                "; install an interface and an engine from the same build";
        return false;
    }
    if (h->peak_slots != kPeakSlots || h->block_bytes != state_block_bytes(h->capacity_tracks) ||
        bytes < h->block_bytes) {
        error = "the engine's state block is " + std::to_string(bytes) +
                " bytes, which does not match the size its header declares";
        return false;
    }
    if (h->track_count.load(std::memory_order_acquire) > h->capacity_tracks) {
        error = "the engine's state block counts more tracks than it has room for";
        return false;
    }
    header_ = h;
    tracks_ = state_tracks(block);
    // From now on, not from the start of the session: a reader that attaches
    // late is not shown every peak since the engine started.
    cursors_.assign(h->capacity_tracks, h->heartbeat.load(std::memory_order_acquire));
    seen_any_ = false;
    return true;
}

uint32_t StateView::track_count() const {
    if (header_ == nullptr) return 0;
    return std::min(header_->track_count.load(std::memory_order_acquire),
                    header_->capacity_tracks);
}

uint64_t StateView::publish_period_ns() const {
    return header_ == nullptr ? 0 : header_->publish_period_ns;
}

TransportSnapshot StateView::transport() const {
    TransportSnapshot out;
    if (header_ == nullptr) {
        out.stale = true;
        return out;
    }
    const StateHeader& h = *header_;
    TransportSnapshot s;
    const bool ok = seqlock_read(h, stuck_seq_, [&]() { read_transport(h, s); });
    if (!ok) {
        ++stale_reads_;
        out.stale = true;
        out.heartbeat = h.heartbeat.load(std::memory_order_acquire);
        return out;
    }
    // Stale rather than draw a fourth transport state.
    if (!known_state(s.state)) {
        out.stale = true;
        return out;
    }
    s.heartbeat = h.heartbeat.load(std::memory_order_acquire);
    return s;
}

TrackSnapshot StateView::track(uint32_t index) const {
    TrackSnapshot out;
    if (header_ == nullptr || index >= track_count()) {
        out.stale = true;
        return out;
    }
    const TrackState& t = tracks_[index];
    TrackSnapshot s;
    const bool ok = seqlock_read(*header_, stuck_seq_, [&]() { read_track(t, s); });
    if (!ok) {
        ++stale_reads_;
        out.stale = true;
        return out;
    }
    read_insert_counters(t, s);
    return s;
}

void StateView::frame(StateFrame& out) const {
    const uint32_t n = track_count();
    out.tracks.resize(n);
    const auto all_stale = [&out]() {
        out.transport = TransportSnapshot{};
        out.transport.stale = true;
        for (TrackSnapshot& t : out.tracks) {
            t = TrackSnapshot{};
            t.stale = true;
        }
    };
    if (header_ == nullptr) {
        all_stale();
        return;
    }
    const StateHeader& h = *header_;
    const bool ok = seqlock_read(h, stuck_seq_, [&]() {
        read_transport(h, out.transport);
        for (uint32_t i = 0; i < n; ++i) read_track(tracks_[i], out.tracks[i]);
    });
    if (!ok || !known_state(out.transport.state)) {
        if (!ok) ++stale_reads_;
        all_stale();
        out.transport.heartbeat = h.heartbeat.load(std::memory_order_acquire);
        return;
    }
    out.transport.stale = false;
    for (uint32_t i = 0; i < n; ++i) {
        out.tracks[i].stale = false;
        read_insert_counters(tracks_[i], out.tracks[i]);
    }
    out.transport.heartbeat = h.heartbeat.load(std::memory_order_acquire);
}

float StateView::take_peak(uint32_t index) {
    if (header_ == nullptr || index >= track_count()) return 0.0f;
    const uint64_t now = header_->heartbeat.load(std::memory_order_acquire);
    uint64_t& cursor = cursors_[index];
    if (now <= cursor) return 0.0f;
    const uint64_t first = std::max(cursor + 1, now >= kPeakSlots ? now - kPeakSlots + 1 : 1);
    float peak = 0.0f;
    const PeakRing& ring = tracks_[index].peaks;
    for (uint64_t tick = first; tick <= now; ++tick) {
        const uint64_t v = ring.entries[tick % kPeakSlots].load(std::memory_order_acquire);
        // A newer tick has taken the slot: this reader fell a whole ring
        // behind while reading. The newer value is read on the next call.
        if ((v >> 32) != (tick & 0xFFFFFFFFull)) continue;
        const uint32_t bits = static_cast<uint32_t>(v & 0xFFFFFFFFull);
        float p = 0.0f;
        std::memcpy(&p, &bits, sizeof p);
        // NaN never wins and never sticks: a meter that shows nothing is
        // honest, one stuck at NaN is not.
        if (p > peak) peak = p;
    }
    cursor = now;
    return peak;
}

bool StateView::engine_alive(uint64_t now_ns) const {
    if (header_ == nullptr) return false;
    const uint64_t hb = header_->heartbeat.load(std::memory_order_acquire);
    if (!seen_any_ || hb != seen_heartbeat_) {
        // The first look has nothing to compare with: alive if the engine
        // has published at all, and judged from here on.
        const bool first = !seen_any_;
        seen_any_ = true;
        seen_heartbeat_ = hb;
        seen_at_ns_ = now_ns;
        return !first || hb > 0;
    }
    const uint64_t timeout = std::max(kMinAliveTimeoutNs, 10 * header_->publish_period_ns);
    if (hb == 0) return false;
    if (now_ns < seen_at_ns_) return true;
    return now_ns - seen_at_ns_ < timeout;
}

}  // namespace sonora::api
