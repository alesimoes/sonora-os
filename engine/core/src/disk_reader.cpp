// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/disk_reader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <tuple>

namespace sonora {

DiskReader::DiskReader(const ReaderConfig& cfg, PlanExchange& exchange,
                       const Transport& transport)
    : chunk_frames_(cfg.chunk_frames),
      window_frames_(cfg.window_frames),
      pool_chunks_(cfg.pool_chunks),
      max_coalesce_(cfg.max_coalesce ? cfg.max_coalesce : 1),
      wake_ms_(cfg.wake_ms),
      exchange_(exchange),
      transport_(transport),
      pool_(new float[static_cast<std::size_t>(cfg.pool_chunks) * cfg.chunk_frames]),
      sources_(new Source[cfg.max_sources]),
      max_sources_(cfg.max_sources),
      buffers_(cfg.pool_chunks) {
    // Every page written once here, so the audio thread never faults one in
    // (ADR-0009 §4.3); mlockall(MCL_FUTURE) keeps them.
    std::memset(pool_.get(), 0,
                static_cast<std::size_t>(pool_chunks_) * chunk_frames_ * sizeof(float));
    free_.reserve(pool_chunks_);
    for (uint32_t i = pool_chunks_; i > 0; --i) free_.push_back(i - 1);
    by_key_.reserve(pool_chunks_ * 2u);
}

DiskReader::~DiskReader() { stop(); }

void DiskReader::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void DiskReader::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_flag_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void DiskReader::wake() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_flag_ = true;
    }
    cv_.notify_all();
}

bool DiskReader::add_source(const std::string& path, uint32_t& slot, std::string& error) {
    const uint32_t n = source_count_.load(std::memory_order_relaxed);
    if (n >= max_sources_) {
        error = "the session's source table is full (" + std::to_string(max_sources_) +
                " sources)";
        return false;
    }
    Source& s = sources_[n];
    if (!s.file.open(path, error)) return false;
    s.channels = s.file.channels();
    s.frames = s.file.frames();
    s.time_reference = s.file.time_reference();
    slot = n;
    // The entry is complete before the reader can see it.
    source_count_.store(n + 1, std::memory_order_release);
    return true;
}

uint64_t DiskReader::serve(PlaybackPlan* playing, PlaybackPlan* pending,
                           int64_t locate_target, uint64_t locate_serial) {
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_.playing = playing;
        request_.pending = pending;
        request_.locate_target = locate_target;
        request_.locate_serial = locate_serial;
        gen = ++request_.generation;
        wake_flag_ = true;
    }
    cv_.notify_all();
    return gen;
}

void DiskReader::after_deactivate() {
    if (!running_.load()) {
        reclaim_aged_after_deactivate();
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t target = ++deactivate_requests_;
    wake_flag_ = true;
    cv_.notify_all();
    done_cv_.wait(lock, [&] { return deactivate_done_ >= target || !running_.load(); });
}

void DiskReader::run_pass() {
    Request req;
    bool deactivated = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        req = request_;
        if (deactivate_done_ != deactivate_requests_) {
            deactivated = true;
            deactivate_done_ = deactivate_requests_;
        }
    }
    if (deactivated) reclaim_aged_after_deactivate();
    pass(req);
}

void DiskReader::run() {
    while (running_.load()) {
        Request req;
        uint64_t deactivate = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(wake_ms_),
                         [this] { return wake_flag_; });
            wake_flag_ = false;
            req = request_;
            deactivate = deactivate_requests_;
        }
        if (!running_.load()) break;
        // Adopt first: a plan dropped before the deactivate is detached, so
        // its buffers count as unlinked before it.
        if (req.generation != adopted_.load(std::memory_order_relaxed)) adopt(req);
        if (deactivate != deactivate_done_) {
            reclaim_aged_after_deactivate();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                deactivate_done_ = deactivate;
            }
            done_cv_.notify_all();
        }
        pass(req);
    }
    // Nobody waits for a pass that will never come.
    done_cv_.notify_all();
}

void DiskReader::adopt(const Request& req) {
    PlaybackPlan* const old[2] = {playing_, pending_};
    for (PlaybackPlan* p : old) {
        if (p && p != req.playing && p != req.pending) detach(p);
    }
    playing_ = req.playing;
    pending_ = req.pending;
    adopted_.store(req.generation, std::memory_order_release);
}

// A plan left the set: retired after an exchange, or a pending plan
// cancelled. Its slots are left as they are -- a retired plan can still be
// in a block's hands -- and only the link counts drop. The blocks_ value read
// here comes after the exchange that retired it, which is all the rule needs.
void DiskReader::detach(PlaybackPlan* plan) {
    for (uint32_t i = 0; i < plan->chunk_slot_count; ++i) {
        const uint32_t b = plan->chunk_slots[i].load(std::memory_order_relaxed);
        if (b == kNoChunk) continue;
        Buffer& buf = buffers_[b];
        if (--buf.links == 0) {
            buf.state = State::Cached;
            buf.unlinked_at = exchange_.blocks();
            buf.aged = false;
            cached_.push_back(b);
            linked_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void DiskReader::link(PlaybackPlan* plan, uint32_t slot, uint32_t buffer) {
    Buffer& buf = buffers_[buffer];
    if (buf.links++ == 0) {
        buf.state = State::Linked;
        linked_.fetch_add(1, std::memory_order_relaxed);
    }
    // The pool data was written before this store, on this thread.
    plan->chunk_slots[slot].store(buffer, std::memory_order_seq_cst);
}

// Takes a live buffer out of every live plan: "empty" into each slot naming
// it, then blocks_ read, in that order and seq_cst (see the header).
void DiskReader::unlink_buffer(uint32_t buffer) {
    Buffer& buf = buffers_[buffer];
    PlaybackPlan* const live[2] = {playing_, pending_};
    for (PlaybackPlan* p : live) {
        if (!p) continue;
        for (uint32_t i = 0; i < p->chunk_slot_count; ++i) {
            if (p->chunk_slots[i].load(std::memory_order_relaxed) != buffer) continue;
            p->chunk_slots[i].store(kNoChunk, std::memory_order_seq_cst);
            --buf.links;
        }
    }
    if (buf.links == 0 && buf.state == State::Linked) {
        buf.state = State::Cached;
        buf.unlinked_at = exchange_.blocks();
        buf.aged = false;
        cached_.push_back(buffer);
        linked_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void DiskReader::reclaim_aged_after_deactivate() {
    for (Buffer& b : buffers_) {
        if (b.state == State::Cached) b.aged = true;
    }
}

uint32_t DiskReader::take_buffer(int64_t playhead) {
    if (!free_.empty()) {
        const uint32_t b = free_.back();
        free_.pop_back();
        return b;
    }
    // Eviction order (ADR-0009 §4.3): first what no plan links, oldest
    // unlink first, once no block can still hold it.
    const uint64_t now = exchange_.blocks();
    while (!cached_.empty()) {
        const uint32_t b = cached_.front();
        Buffer& buf = buffers_[b];
        if (buf.state != State::Cached) {
            cached_.pop_front();  // linked again since
            continue;
        }
        if (!buf.aged && buf.unlinked_at == now) break;
        cached_.pop_front();
        by_key_.erase(buf.key);
        buf.state = State::Free;
        return b;
    }
    // Then the playing plan's chunks behind the playhead. They become
    // reusable once a block has passed, so this pass goes without.
    evict_behind(playhead);
    cache_full_.fetch_add(1, std::memory_order_relaxed);
    return kNoChunk;
}

bool DiskReader::protected_chunk(const PlanSegment& seg, uint64_t chunk) const {
    const int64_t src_from = static_cast<int64_t>(chunk) * chunk_frames_;
    const int64_t from = std::max(seg.start, seg.start + (src_from - seg.source_offset));
    const int64_t to = std::min(seg.end, from + chunk_frames_);
    for (int i = 0; i < protect_count_; ++i) {
        if (from < protect_[i].to && to > protect_[i].from) return true;
    }
    return false;
}

// Never a chunk the playing plan needs in [playhead, playhead + W) or at a
// locate target in flight; those are exactly the protected ranges. A buffer
// is evicted only if no slot that names it, in either live plan, is
// protected or ahead of the playhead.
void DiskReader::evict_behind(int64_t playhead) {
    if (!playing_) return;
    uint32_t evicted = 0;
    for (uint32_t si = 0; si < playing_->segment_count && evicted < max_coalesce_; ++si) {
        const PlanSegment& seg = playing_->segments[si];
        if (seg.start >= playhead) break;
        for (uint32_t c = 0; c < seg.channels; ++c) {
            for (uint32_t k = 0; k < seg.chunk_count[c]; ++k) {
                const uint32_t slot = seg.first_chunk_slot[c] + k;
                const uint32_t b = playing_->chunk_slots[slot].load(std::memory_order_relaxed);
                if (b == kNoChunk) continue;
                bool keep = false;
                PlaybackPlan* const live[2] = {playing_, pending_};
                for (PlaybackPlan* p : live) {
                    if (!p || keep) continue;
                    for (uint32_t sj = 0; sj < p->segment_count && !keep; ++sj) {
                        const PlanSegment& s2 = p->segments[sj];
                        for (uint32_t c2 = 0; c2 < s2.channels && !keep; ++c2) {
                            for (uint32_t k2 = 0; k2 < s2.chunk_count[c2]; ++k2) {
                                const uint32_t slot2 = s2.first_chunk_slot[c2] + k2;
                                if (p->chunk_slots[slot2].load(std::memory_order_relaxed) != b) {
                                    continue;
                                }
                                const uint64_t chunk = s2.first_chunk_index[c2] + k2;
                                const int64_t end = std::min(
                                    s2.end, s2.start + (static_cast<int64_t>(chunk + 1) *
                                                            chunk_frames_ -
                                                        s2.source_offset));
                                if (end > playhead || protected_chunk(s2, chunk)) {
                                    keep = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (keep) continue;
                unlink_buffer(b);
                if (++evicted >= max_coalesce_) return;
            }
        }
    }
}

namespace {

// Calls fn(segment, channel, chunk, slot) for every chunk slot `plan` reads
// while playing [from, to).
template <typename Fn>
void for_each_window_slot(const PlaybackPlan* plan, int64_t from, int64_t to,
                          uint32_t chunk_frames, Fn&& fn) {
    for (uint32_t si = 0; si < plan->segment_count; ++si) {
        const PlanSegment& seg = plan->segments[si];
        if (seg.end <= from || seg.start >= to) continue;
        const int64_t a = std::max(from, seg.start);
        const int64_t b = std::min(to, seg.end);
        const int64_t s0 = seg.source_offset + (a - seg.start);
        const int64_t s1 = seg.source_offset + (b - seg.start);
        const uint64_t k0 = static_cast<uint64_t>(s0) / chunk_frames;
        const uint64_t k1 = static_cast<uint64_t>(s1 - 1) / chunk_frames;
        for (uint32_t c = 0; c < seg.channels; ++c) {
            for (uint64_t k = k0; k <= k1; ++k) {
                const uint32_t slot = chunk_slot_of(seg, c, k);
                if (slot != kNoChunk) fn(seg, c, k, slot);
            }
        }
    }
}

}  // namespace

void DiskReader::fill(PlaybackPlan* plan, int64_t from) {
    if (!plan) return;
    const int64_t to = from + window_frames_;
    struct Missing {
        uint32_t source;
        uint64_t chunk;
        uint16_t channel;
        bool operator<(const Missing& o) const {
            return std::tie(source, chunk, channel) < std::tie(o.source, o.chunk, o.channel);
        }
        bool operator==(const Missing& o) const {
            return source == o.source && chunk == o.chunk && channel == o.channel;
        }
    };
    std::vector<Missing> missing;
    const auto link_or_note = [&](bool note) {
        for_each_window_slot(plan, from, to, chunk_frames_,
                             [&](const PlanSegment& seg, uint32_t c, uint64_t k, uint32_t slot) {
            if (plan->chunk_slots[slot].load(std::memory_order_relaxed) != kNoChunk) return;
            const auto it = by_key_.find(key_of(seg.source_slot[c], seg.source_channel[c], k));
            if (it != by_key_.end()) {
                link(plan, slot, it->second);
            } else if (note) {
                missing.push_back({seg.source_slot[c], k, seg.source_channel[c]});
            }
        });
    };
    link_or_note(true);
    if (missing.empty()) return;
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

    // Runs of adjacent chunks of one source, every channel any of them needs.
    std::vector<uint16_t> channels;
    std::size_t i = 0;
    while (i < missing.size()) {
        const uint32_t source = missing[i].source;
        const uint64_t first = missing[i].chunk;
        uint64_t last = first;
        channels.clear();
        std::size_t j = i;
        while (j < missing.size() && missing[j].source == source &&
               missing[j].chunk <= last + 1 && missing[j].chunk - first < max_coalesce_) {
            last = missing[j].chunk;
            if (std::find(channels.begin(), channels.end(), missing[j].channel) == channels.end()) {
                channels.push_back(missing[j].channel);
            }
            ++j;
        }
        read_run(source, first, static_cast<uint32_t>(last - first + 1), channels,
                 transport_.playhead());
        i = j;
    }
    link_or_note(false);
}

void DiskReader::read_run(uint32_t source, uint64_t first_chunk, uint32_t count,
                          const std::vector<uint16_t>& channels, int64_t playhead) {
    if (source >= source_count_.load(std::memory_order_acquire)) return;
    Source& src = sources_[source];
    // Buffers first: a read with nowhere to put it is wasted I/O. When the
    // pool runs short the run is cut to the whole chunks it has room for.
    std::vector<uint32_t> got;
    got.reserve(static_cast<std::size_t>(count) * channels.size());
    for (uint32_t k = 0; k < count; ++k) {
        const std::size_t before = got.size();
        for (std::size_t c = 0; c < channels.size(); ++c) {
            const uint32_t b = take_buffer(playhead);
            if (b == kNoChunk) break;
            got.push_back(b);
        }
        if (got.size() - before < channels.size()) {
            for (std::size_t n = before; n < got.size(); ++n) {
                buffers_[got[n]].state = State::Free;
                free_.push_back(got[n]);
            }
            got.resize(before);
            count = k;
            break;
        }
    }
    if (count == 0) return;

    const std::size_t frames = static_cast<std::size_t>(count) * chunk_frames_;
    read_scratch_.assign(frames * src.channels, 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t n = src.file.read(static_cast<int64_t>(first_chunk) * chunk_frames_,
                                    static_cast<int64_t>(frames), read_scratch_.data());
    const auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - t0)
                                              .count());
    reads_.fetch_add(1, std::memory_order_relaxed);
    if (us > max_read_us_.load(std::memory_order_relaxed)) {
        max_read_us_.store(us, std::memory_order_relaxed);
    }
    if (n < 0) {
        read_errors_.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t r : got) {
            buffers_[r].state = State::Free;
            free_.push_back(r);
        }
        return;
    }
    // Past the end of the source the scratch is still zero: silence.
    std::size_t g = 0;
    for (uint32_t k = 0; k < count; ++k) {
        for (const uint16_t ch : channels) {
            const uint32_t b = got[g++];
            float* dst = pool_.get() + static_cast<std::size_t>(b) * chunk_frames_;
            const float* in = read_scratch_.data() +
                              static_cast<std::size_t>(k) * chunk_frames_ * src.channels +
                              (ch < src.channels ? ch : 0);
            if (ch < src.channels) {
                for (uint32_t f = 0; f < chunk_frames_; ++f) dst[f] = in[f * src.channels];
            } else {
                std::memset(dst, 0, chunk_frames_ * sizeof(float));
            }
            Buffer& buf = buffers_[b];
            buf.key = key_of(source, ch, first_chunk + k);
            buf.links = 0;
            buf.aged = false;
            // Not on cached_ until something unlinks it: nothing can take it
            // before fill() links it.
            buf.state = State::Cached;
            by_key_[buf.key] = b;
        }
    }
    chunks_read_.fetch_add(count, std::memory_order_relaxed);
}

bool DiskReader::window_resident(const PlaybackPlan* plan, int64_t from) const {
    if (!plan) return true;
    bool all = true;
    for_each_window_slot(plan, from, from + window_frames_, chunk_frames_,
                         [&](const PlanSegment&, uint32_t, uint64_t, uint32_t slot) {
        if (plan->chunk_slots[slot].load(std::memory_order_relaxed) == kNoChunk) all = false;
    });
    return all;
}

void DiskReader::pass(const Request& req) {
    if (req.generation != adopted_.load(std::memory_order_relaxed)) adopt(req);
    // The locate count first: a playhead read after it is at or past the
    // locate it reports.
    const uint64_t applied = transport_.locates_applied();
    const auto playhead = static_cast<int64_t>(transport_.playhead());
    const bool locating = req.locate_serial != 0 && applied < req.locate_serial;

    protect_count_ = 0;
    protect_[protect_count_++] = {playhead, playhead + window_frames_};
    if (locating) {
        protect_[protect_count_++] = {req.locate_target, req.locate_target + window_frames_};
    }

    // What is heard now first, then what gates the next swap or locate.
    fill(playing_, playhead);
    if (locating) fill(playing_, req.locate_target);
    fill(pending_, playhead);
    if (locating) fill(pending_, req.locate_target);

    bool ok = window_resident(pending_, playhead);
    if (locating) {
        ok = ok && window_resident(playing_, req.locate_target) &&
             window_resident(pending_, req.locate_target);
    }
    if (ok) resident_.store(req.generation, std::memory_order_release);
}

}  // namespace sonora
