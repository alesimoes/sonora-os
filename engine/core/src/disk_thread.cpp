// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/disk_thread.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "sonora/track.h"

namespace sonora {

static_assert(Track::kWriterSlots == 2, "TrackState keeps a wrap list per writer slot");

DiskThread::DiskThread(std::vector<Track*> tracks, std::size_t chunk_frames,
                       RingBuffer<CaptureBoundary>& boundaries, std::size_t wrap_cap)
    : tracks_(std::move(tracks)),
      chunk_frames_(chunk_frames),
      wrap_cap_(wrap_cap),
      boundaries_(boundaries),
      state_(tracks_.size()) {
    std::size_t channels = 1;
    for (const Track* t : tracks_) channels = std::max<std::size_t>(channels, t->channels());
    scratch_.resize(chunk_frames_ * channels);
    for (TrackState& s : state_) {
        for (auto& list : s.wraps) list.reserve(wrap_cap_);
    }
}

DiskThread::~DiskThread() { stop(); }

void DiskThread::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&DiskThread::run, this);
}

void DiskThread::stop() {
    if (running_.exchange(false) && thread_.joinable()) thread_.join();
    // One final pass so nothing still sitting in a ring is lost.
    drain_once();
}

uint32_t DiskThread::total_overruns() const {
    uint32_t sum = 0;
    for (const Track* t : tracks_) sum += t->overruns();
    return sum;
}

std::vector<DiskThread::ClosedFile> DiskThread::take_closed() {
    std::lock_guard<std::mutex> lock(closed_mutex_);
    std::vector<ClosedFile> out;
    out.swap(closed_);
    return out;
}

void DiskThread::run() {
    auto next_checkpoint = std::chrono::steady_clock::now() + checkpoint_interval_;
    while (running_.load(std::memory_order_relaxed)) {
        drain_once();
        // After a drain, so the checkpoint covers what the rings held.
        // From the time it ends: a slow sync pushes the next one back
        // rather than letting checkpoints run back to back.
        if (std::chrono::steady_clock::now() >= next_checkpoint) {
            checkpoint_all();
            next_checkpoint = std::chrono::steady_clock::now() + checkpoint_interval_;
        }
        // Wake often enough that the rings never approach full, but not so
        // often that we spin. Rings hold seconds; this polls every 10 ms.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void DiskThread::drain_once() {
    // The fill first, then the records. A record is pushed after the
    // samples before it, so every sample a record read here refers to is
    // in its ring. And a window's samples are written after its open
    // record, so every sample counted here has its record among those read
    // below: read the other way round, samples written in between would be
    // drained before their window opened, and dropped.
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        state_[i].visible = state_[i].consumed + tracks_[i]->capture_ring().read_space();
    }
    CaptureBoundary b{};
    while (boundaries_.read(&b, 1) == 1) {
        if (b.track < state_.size()) state_[b.track].pending.push_back(b);
    }
    if (hook_) hook_(hook_context_);
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        drain_track(i);
        // Nothing drained stays in this process, where a kill -9 would take
        // it: only what is still in the ring is lost with it.
        if (state_[i].active >= 0) tracks_[i]->writer(state_[i].active).flush();
    }
}

void DiskThread::drain_track(std::size_t i) {
    TrackState& s = state_[i];
    for (;;) {
        // Its `at` may be ahead of every sample yet pushed, and it splits
        // nothing, so it never waits for its audio: a Close behind it must
        // not wait either.
        if (!s.pending.empty() && s.pending.front().kind == CaptureBoundary::Kind::Wrap) {
            wrap(i, s.pending.front());
            s.pending.pop_front();
            continue;
        }
        // A record read this pass may lie past the fill read before it; the
        // samples up to it are drained next pass.
        uint64_t n = s.visible - s.consumed;
        if (!s.pending.empty()) {
            const uint64_t at = s.pending.front().at;
            n = at > s.consumed ? std::min<uint64_t>(n, at - s.consumed) : 0;
        }
        if (n > 0) move(i, n);
        if (s.pending.empty() || s.consumed < s.pending.front().at) return;
        apply(i, s.pending.front());
        s.pending.pop_front();
    }
}

void DiskThread::move(std::size_t i, uint64_t n) {
    TrackState& s = state_[i];
    Track& t = *tracks_[i];
    const std::size_t ch = t.channels();
    const std::size_t chunk = scratch_.size() / ch * ch;
    while (n > 0) {
        const std::size_t samples = static_cast<std::size_t>(std::min<uint64_t>(n, chunk));
        t.capture_ring().read(scratch_.data(), samples);
        s.consumed += samples;
        n -= samples;
        // Only whole frames, so a file never desynchronises. A false return
        // is the writer marking the take failed (disk error, 4 GB limit);
        // the ring is still drained, so the callback does not count
        // overruns for a take that is already lost.
        if (s.active >= 0) {
            t.writer(s.active).write(scratch_.data(), samples / ch);
        } else {
            dropped_.fetch_add(samples, std::memory_order_relaxed);
        }
    }
}

void DiskThread::apply(std::size_t i, const CaptureBoundary& b) {
    TrackState& s = state_[i];
    Track& t = *tracks_[i];
    if (b.kind == CaptureBoundary::Kind::Close) {
        if (s.active >= 0 && t.slot_take(s.active) == static_cast<int>(b.take)) {
            finish(i, s.active);
        }
        return;
    }
    // An open window whose close record was lost to a full boundary ring
    // ends where the next one starts.
    if (s.active >= 0) finish(i, s.active);
    for (int slot = 0; slot < Track::kWriterSlots; ++slot) {
        const int take = t.slot_take(slot);
        // Opened for an earlier take whose window never opened here: the
        // track was disarmed before its first block, or the record was lost.
        // "Earlier" is "smaller" because take numbers only increase: every
        // take is numbered by next_free_take(), one past the highest on
        // disk, and start_recording() puts its files on disk.
        if (take != 0 && take < static_cast<int>(b.take)) finish(i, slot);
    }
    // Take 0 has no file: tests drive the ring with it. A take whose files
    // stop() has already closed, its callback late, finds none either, and
    // its samples are read and dropped.
    for (int slot = 0; b.take != 0 && slot < Track::kWriterSlots; ++slot) {
        if (t.slot_take(slot) == static_cast<int>(b.take)) {
            s.active = slot;
            s.open_at = b.at;
            t.writer(slot).set_time_reference(b.timeline);
            return;
        }
    }
}

void DiskThread::wrap(std::size_t i, const CaptureBoundary& b) {
    TrackState& s = state_[i];
    Track& t = *tracks_[i];
    if (s.active < 0 || t.slot_take(s.active) != static_cast<int>(b.take)) return;
    const int slot = s.active;
    std::vector<uint64_t>& list = s.wraps[slot];
    uint64_t& written = s.wraps_written[slot];
    if (b.ordinal <= written) return;
    WrapSidecar& side = t.sidecar(slot);
    // The ring counts samples; the list counts the file's frames.
    const uint64_t frame = (b.at > s.open_at ? b.at - s.open_at : 0) / t.channels();
    // Wraps whose records a full ring lost keep their places, so every
    // later wrap is still the pass it ends.
    while (written + 1 < b.ordinal) {
        if (list.size() < wrap_cap_) list.push_back(WrapSidecar::kLostWrap);
        side.append(WrapSidecar::kLostWrap);
        ++written;
    }
    // Past the cap, on disk only: those passes stay recoverable.
    if (list.size() < wrap_cap_) list.push_back(frame);
    side.append(frame);
    ++written;
}

void DiskThread::finish(std::size_t i, int slot) {
    Track& t = *tracks_[i];
    WavWriter& w = t.writer(slot);
    w.close();
    ClosedFile f;
    f.track = static_cast<int>(i);
    f.take = t.slot_take(slot);
    f.path = w.path();
    f.frames = w.frames_written();
    if (w.failed()) f.failure = w.failure();
    WrapSidecar& side = t.sidecar(slot);
    if (side.is_open()) {
        side.close();
        f.wrap_failure = side.failure();
    }
    std::vector<uint64_t>& list = state_[i].wraps[slot];
    f.wraps = list;
    f.wraps_past_cap = state_[i].wraps_written[slot] - list.size();
    // Keeps its capacity, so the next take's list does not grow it.
    list.clear();
    state_[i].wraps_written[slot] = 0;
    {
        std::lock_guard<std::mutex> lock(closed_mutex_);
        closed_.push_back(std::move(f));
    }
    if (state_[i].active == slot) state_[i].active = -1;
    // Last: from here the control thread may open the slot again.
    t.set_slot_take(slot, 0);
}

void DiskThread::checkpoint_all() {
    for (Track* t : tracks_) {
        for (int slot = 0; slot < Track::kWriterSlots; ++slot) {
            // A slot with a take is this thread's (Track::slot_take()),
            // whether or not its window has opened yet. A failure marks the
            // take failed, which Session::take_failure() reports.
            if (t->slot_take(slot) == 0 || !t->writer(slot).is_open()) continue;
            const auto begin = std::chrono::steady_clock::now();
            t->writer(slot).checkpoint();
            // After the take's own sync, so no wrap is on the medium ahead of
            // the audio it ends (ADR-0016 C1).
            if (t->sidecar(slot).is_open()) t->sidecar(slot).checkpoint();
            const auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begin)
                    .count());
            if (ns > longest_checkpoint_ns_.load(std::memory_order_relaxed)) {
                longest_checkpoint_ns_.store(ns, std::memory_order_relaxed);
            }
            // Between files, so the rings wait for one sync and never for
            // the sum of them: 18 tracks on a slow stick would come close to
            // the ring's depth. A file this drain closes is skipped above.
            drain_once();
        }
    }
    checkpoints_.fetch_add(1, std::memory_order_relaxed);
}

void DiskThread::close_all() {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (state_[i].active >= 0) finish(i, state_[i].active);
        for (int slot = 0; slot < Track::kWriterSlots; ++slot) {
            if (tracks_[i]->slot_take(slot) != 0) finish(i, slot);
        }
    }
}

}  // namespace sonora
