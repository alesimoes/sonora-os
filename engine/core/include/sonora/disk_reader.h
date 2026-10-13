// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playback disk reader and its source chunk cache (ADR-0009 §4.3, §4.4;
// timeline-model.md, "When a plan may be published: residency first").
//
// A non-real-time thread, separate from the capture writer so a slow read
// never delays a capture drain. It loads fixed-size chunks of source audio,
// converted to float, into a pool allocated and pre-faulted with the
// session, and links each into the chunk slots of the plans that play it:
// the playing plan and at most one pending plan. Adjacent chunks of one
// source are read in one coalesced read.
//
// The audio thread reads a slot, then the pool buffer it names. A slot found
// empty is silence and a counted miss, never a wait.
//
// Reclaiming a buffer follows the plan rule (ADR-0009 §4.1): the reader
// stores "empty" into every slot naming it, then reads s = blocks_, and
// reuses the buffer only once blocks_ != s. Both the store and the audio
// thread's load of a slot are seq_cst, with blocks_'s store and load: that is
// what puts the unlink and the block's read of the slot in one total order,
// so a block that saw the buffer stored its count before s was read.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sonora/plan_exchange.h"
#include "sonora/playback_plan.h"
#include "sonora/rt.h"
#include "sonora/transport.h"
#include "sonora/wav_reader.h"

namespace sonora {

struct ReaderConfig {
    // ADR-0009 Unverified 1 settles both; until then these are placeholders.
    uint32_t chunk_frames = 16384;
    int64_t window_frames = 4 * 48000;
    uint32_t pool_chunks = 64;
    uint32_t max_sources = 1024;
    // Most chunks of one source read in one call.
    uint32_t max_coalesce = 16;
    // How often the thread looks again when nothing woke it.
    uint32_t wake_ms = 5;
};

class DiskReader {
public:
    DiskReader(const ReaderConfig& cfg, PlanExchange& exchange, const Transport& transport);
    ~DiskReader();

    DiskReader(const DiskReader&) = delete;
    DiskReader& operator=(const DiskReader&) = delete;

    void start();
    void stop();
    // One pass of the thread's loop on the caller's thread, for tests. Only
    // while the thread is not running.
    void run_pass();

    // --- control thread ---
    // Opens a source and appends it to the table plans index; `slot` is its
    // index. The table is append-only and fixed in size.
    bool add_source(const std::string& path, uint32_t& slot, std::string& error);
    uint32_t source_count() const { return source_count_.load(std::memory_order_acquire); }
    int64_t source_frames(uint32_t slot) const { return sources_[slot].frames; }
    uint16_t source_channels(uint32_t slot) const { return sources_[slot].channels; }
    uint64_t source_time_reference(uint32_t slot) const { return sources_[slot].time_reference; }

    // The plans to serve from the next pass: `playing` is the plan the audio
    // thread reads, `pending` the one waiting for residency (or null). With
    // locate_serial non-zero, the window at locate_target is made resident
    // for both and protected until the transport has applied that locate.
    // Returns this request's generation. A plan dropped from the set is no
    // longer touched once adopted() reaches the generation that dropped it.
    uint64_t serve(PlaybackPlan* playing, PlaybackPlan* pending,
                   int64_t locate_target = 0, uint64_t locate_serial = 0);
    uint64_t adopted() const { return adopted_.load(std::memory_order_acquire); }
    // The newest generation whose pending plan and locate window were found
    // resident: every chunk from the playhead (and the locate target)
    // through the window linked.
    uint64_t resident() const { return resident_.load(std::memory_order_acquire); }
    // Wakes the thread now rather than at its next tick.
    void wake();

    // After AudioBackend::deactivate() has returned: no block runs, so every
    // buffer unlinked so far is reusable now. Waits for the thread's next
    // pass if it is running.
    void after_deactivate();

    // --- audio thread ---
    const float* chunk_data(uint32_t buffer) const noexcept SONORA_RT {
        return pool_.get() + static_cast<std::size_t>(buffer) * chunk_frames_;
    }
    uint32_t chunk_frames() const { return chunk_frames_; }

    // --- diagnostics, any thread ---
    uint32_t pool_chunks() const { return pool_chunks_; }
    // Passes that wanted a buffer and found none free or reclaimable.
    uint64_t cache_full() const { return cache_full_.load(std::memory_order_relaxed); }
    uint64_t chunks_read() const { return chunks_read_.load(std::memory_order_relaxed); }
    uint64_t reads() const { return reads_.load(std::memory_order_relaxed); }
    uint64_t read_errors() const { return read_errors_.load(std::memory_order_relaxed); }
    // The slowest coalesced read so far, in microseconds.
    uint64_t max_read_us() const { return max_read_us_.load(std::memory_order_relaxed); }
    // Buffers linked by a plan slot now; the rest are free or cached.
    uint32_t linked_buffers() const { return linked_.load(std::memory_order_relaxed); }

private:
    struct Source {
        WavReader file;
        uint16_t channels = 0;
        int64_t frames = 0;
        uint64_t time_reference = 0;
    };

    enum class State : uint8_t { Free, Linked, Cached };
    struct Buffer {
        uint64_t key = 0;
        uint32_t links = 0;           // plan slots naming it
        uint64_t unlinked_at = 0;     // blocks_ read after the last unlink
        bool aged = false;            // unlinked before a deactivate
        State state = State::Free;
    };

    struct Request {
        PlaybackPlan* playing = nullptr;
        PlaybackPlan* pending = nullptr;
        int64_t locate_target = 0;
        uint64_t locate_serial = 0;
        uint64_t generation = 0;
    };

    struct Range {
        int64_t from;
        int64_t to;
    };

    void run();
    void pass(const Request& req);
    void adopt(const Request& req);
    void detach(PlaybackPlan* plan);
    void link(PlaybackPlan* plan, uint32_t slot, uint32_t buffer);
    void unlink_buffer(uint32_t buffer);
    // Links or reads every chunk `plan` plays in [from, from + window).
    void fill(PlaybackPlan* plan, int64_t from);
    bool window_resident(const PlaybackPlan* plan, int64_t from) const;
    uint32_t take_buffer(int64_t playhead);
    void evict_behind(int64_t playhead);
    bool protected_chunk(const PlanSegment& seg, uint64_t chunk) const;
    void read_run(uint32_t source, uint64_t first_chunk, uint32_t count,
                  const std::vector<uint16_t>& channels, int64_t playhead);
    void reclaim_aged_after_deactivate();

    static uint64_t key_of(uint32_t source, uint16_t channel, uint64_t chunk) {
        return (static_cast<uint64_t>(source) << 44) | (static_cast<uint64_t>(channel) << 32) |
               (chunk & 0xFFFFFFFFu);
    }

    const uint32_t chunk_frames_;
    const int64_t window_frames_;
    const uint32_t pool_chunks_;
    const uint32_t max_coalesce_;
    const uint32_t wake_ms_;
    PlanExchange& exchange_;
    const Transport& transport_;

    std::unique_ptr<float[]> pool_;
    std::unique_ptr<Source[]> sources_;
    const uint32_t max_sources_;
    std::atomic<uint32_t> source_count_{0};

    // Reader-thread state.
    std::vector<Buffer> buffers_;
    std::vector<uint32_t> free_;
    std::deque<uint32_t> cached_;   // unlink order; stale entries skipped
    std::unordered_map<uint64_t, uint32_t> by_key_;
    std::vector<float> read_scratch_;
    PlaybackPlan* playing_ = nullptr;
    PlaybackPlan* pending_ = nullptr;
    Range protect_[3] = {};
    int protect_count_ = 0;

    // Control <-> reader mailbox. Never touched by the audio thread.
    std::mutex mutex_;
    std::condition_variable cv_;
    Request request_;
    bool wake_flag_ = false;
    uint64_t deactivate_requests_ = 0;
    uint64_t deactivate_done_ = 0;
    std::condition_variable done_cv_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> adopted_{0};
    std::atomic<uint64_t> resident_{0};
    std::atomic<uint64_t> cache_full_{0};
    std::atomic<uint64_t> chunks_read_{0};
    std::atomic<uint64_t> reads_{0};
    std::atomic<uint64_t> read_errors_{0};
    std::atomic<uint64_t> max_read_us_{0};
    std::atomic<uint32_t> linked_{0};
};

// The audio thread's read of a slot: seq_cst, see above.
inline uint32_t load_chunk_slot(const PlaybackPlan& plan, uint32_t slot) noexcept SONORA_RT {
    return plan.chunk_slots[slot].load(std::memory_order_seq_cst);
}

}  // namespace sonora
