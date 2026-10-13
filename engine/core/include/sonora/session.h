// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sonora/disk_reader.h"
#include "sonora/disk_thread.h"
#include "sonora/plan_exchange.h"
#include "sonora/process_callback.h"
#include "sonora/ringbuffer.h"
#include "sonora/rt.h"
#include "sonora/track.h"
#include "sonora/transport.h"

namespace sonora {

// The playback path's fixed maxima and cache sizing (ADR-0009 §4.3, §4.5).
// Everything here is allocated when the session is created; a plan that
// needs more is refused by the builder, naming the limit.
struct PlaybackConfig {
    // ADR-0009 Unverified 1 settles both; placeholders until then.
    uint32_t chunk_frames = 16384;
    double window_seconds = 4.0;
    // 0: two plans' windows for every track channel, plus a margin.
    uint32_t pool_chunks = 0;
    uint32_t max_sources = 1024;
    uint32_t max_segments = 65536;          // per plan
    uint32_t max_chunk_slots = 1u << 20;    // per plan
    // A larger block is mixed in pieces of this size.
    uint32_t max_block_frames = 1024;
    // Per-edge delay (PDC, Phase 4). 0 allocates no delay line.
    uint32_t max_delay_frames = 0;
    uint32_t command_capacity = 64;
    // The pan law of the plan the session starts with, before engine/control
    // publishes one: session-format.md's default. A different law in the
    // first published plan would be heard as a ramp in its first block.
    float pan_law_db = -3.0f;
};

// A sample-accurate event for the audio thread, drained at block start
// (ADR-0007 §2). Trivial, so the ring can hold it as bytes.
struct RtCommand {
    enum class Kind : uint32_t { None = 0, Locate = 1 };
    Kind kind;
    uint64_t frame;
};

struct SessionConfig {
    std::string name = "Untitled";
    std::string dir;                 // session folder; audio goes in <dir>/Audio Files
    uint32_t sample_rate = 48000;
    int track_count = 8;
    uint16_t channels_per_track = 1;
    WavWriter::Format format = WavWriter::Format::Int24;
    // Seconds of headroom in each capture ring before an overrun is possible.
    double ring_seconds = 4.0;
    PlaybackConfig playback;
};

// Owns the tracks, the transport and the disk thread. The audio backend calls
// process() from its own real-time thread, through ProcessCallback; it never
// sees Session itself. Final because the base has no virtual destructor.
class Session final : public backend::ProcessCallback {
public:
    // The highest take number a session writes. Four digits is far more
    // takes than a session holds, and a bound is what lets the folder scan
    // ignore a stray "Audio 1_20261012.wav" instead of numbering after it.
    static constexpr int kMaxTake = 9999;

    explicit Session(SessionConfig cfg);
    ~Session();

    bool create_dirs();

    // One more than the highest take number among the files in the session's
    // audio folder that are named exactly as start_recording() names them
    // for any track the session could have -- "Audio 12_05.wav" counts even
    // when the session is reopened with 8 tracks -- so a take is never
    // numbered like one already on disk and no stray file can push the
    // count. Can return kMaxTake + 1, which start_recording() refuses with
    // a reason.
    // Reads the directory: never call it from the audio thread.
    int next_free_take() const;

    // Opens a file per armed track and flips the transport to Recording.
    // All or nothing: if any armed track's file cannot be created -- above
    // all because it already exists -- nothing is recorded, the files this
    // call created are removed, and `error`, if given, says which file and
    // why.
    bool start_recording(int take_number, std::string* error = nullptr);
    // Closes every open file and stops the transport. Returns false if the
    // take it ends failed on any track -- a disk error, or the file reaching
    // the 4 GB WAV limit -- and `error`, if given, says which file and why.
    // The files are closed either way, and one that reached the limit is
    // valid up to its last whole frame.
    bool stop(std::string* error = nullptr);
    // While a take runs: empty, or which file failed and why, in the words
    // stop() will use. Writing stops at the failure, so the operator is told
    // now rather than when the take ends. Control thread; allocates.
    std::string take_failure() const;

    // Transport while not recording. play() rolls from the playhead;
    // stop_playback() stops a roll and leaves a take alone (stop() ends
    // one). Control thread.
    void play();
    void stop_playback();

    // Queues a locate for the audio thread, which applies it at the start of
    // a block. False if the queue is full. engine/control sends it only once
    // the target's window is resident (ADR-0009 §4.4).
    bool post_locate(uint64_t frame);

    // Monitor bus gain, linear. Mix state, ramped per block.
    void set_bus_gain(float g) { bus_gain_.store(g, std::memory_order_relaxed); }
    float bus_gain() const { return bus_gain_.load(std::memory_order_relaxed); }
    // Bus peak since the last read, per side; reading clears it.
    float take_bus_peak(int side) {
        return bus_peak_[side & 1].exchange(0.0f, std::memory_order_relaxed);
    }
    // Spans the audio thread played as silence because their chunk was not
    // resident. Each one is an audible hole.
    uint64_t playback_misses() const { return misses_.load(std::memory_order_relaxed); }

    // --- real-time safe: called from the audio callback ---
    // block.inputs[i] feeds track i; a null input, or a track past
    // block.input_count, is unconnected. The current plan's graph mixes
    // every track into the monitor bus, which goes to
    // outputs[plan.monitor_out[0..1]]; every other output is written silent.
    void process(const backend::AudioBlock& block) noexcept SONORA_RT override;

    Transport& transport() { return *transport_; }
    const std::vector<std::unique_ptr<Track>>& tracks() const { return tracks_; }
    const SessionConfig& config() const { return cfg_; }
    DiskThread& disk() { return *disk_; }
    // The plan exchange and the disk reader, for engine/control's publisher.
    PlanExchange& plans() { return *plans_; }
    DiskReader& reader() { return *reader_; }
    int64_t window_frames() const { return window_frames_; }

private:
    void mix(const PlaybackPlan& plan, const backend::AudioBlock& block, uint32_t offset,
             uint32_t n, bool rolling, bool recording, uint64_t playhead) noexcept SONORA_RT;
    void render_track(const PlaybackPlan& plan, uint32_t plan_track, uint64_t from,
                      uint32_t n, float* const out[2]) noexcept SONORA_RT;
    void render_segment(const PlaybackPlan& plan, const PlanSegment& seg, uint64_t from,
                        uint32_t n, float* const out[2]) noexcept SONORA_RT;
    float* scratch(uint32_t buffer, uint32_t side) noexcept SONORA_RT {
        return scratch_.get() + (static_cast<std::size_t>(buffer) * 2 + side) * max_block_;
    }

    SessionConfig cfg_;
    std::unique_ptr<Transport> transport_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::unique_ptr<DiskThread> disk_;

    // Declared before the reader, so the reader is gone before the plans it
    // serves are freed.
    std::unique_ptr<PlanExchange> plans_;
    std::unique_ptr<DiskReader> reader_;
    int64_t window_frames_ = 0;
    std::unique_ptr<RingBuffer<RtCommand>> commands_;

    // Audio-thread state, sized here and never resized.
    uint32_t max_block_ = 0;
    std::unique_ptr<float[]> scratch_;       // (nodes) x 2 x max_block
    std::unique_ptr<float[]> track_buf_;     // 2 x max_block
    std::unique_ptr<float[]> delay_;         // edges x 2 x (max_delay + 1)
    std::unique_ptr<uint32_t[]> delay_pos_;  // per delay line
    uint32_t delay_len_ = 0;
    std::unique_ptr<uint32_t[]> cursor_;     // per plan track
    std::unique_ptr<float[]> track_gain_;    // per engine track, L and R, end of last block
    std::unique_ptr<float[]> track_target_;  // this block's targets
    float bus_applied_ = 1.0f;
    float bus_target_ = 1.0f;
    uint64_t seen_epoch_ = 0;
    uint64_t expected_playhead_ = 0;
    bool reseek_ = true;
    uint32_t block_frames_ = 0;              // the whole block, for ramps
    uint32_t block_done_ = 0;                // frames of it already mixed

    std::atomic<float> bus_gain_{1.0f};
    std::atomic<float> bus_peak_[2] = {{0.0f}, {0.0f}};
    std::atomic<uint64_t> misses_{0};
};

}  // namespace sonora
