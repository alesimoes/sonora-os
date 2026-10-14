// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sonora/disk_reader.h"
#include "sonora/disk_thread.h"
#include "sonora/insert_chain_runner.h"
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
    // Plugin delay compensation (ADR-0012 §7.3): the most one edge is
    // delayed, and the most edges one plan delays (auditor L3). 0 frames
    // allocates no delay line. The pool is never larger than the most edges
    // a plan of this graph can delay, one per track into the bus.
    uint32_t max_delay_frames = 16384;
    uint32_t max_delayed_edges = 128;
    uint32_t command_capacity = 64;
    // Plugin parameter changes queued for the audio thread (ADR-0012 §5.1),
    // in a ring of their own so a knob drag never holds a locate back. The
    // ring is drained every callback, so this bounds what can be posted in
    // one period, across every chain.
    uint32_t plugin_param_capacity = 256;
    // The pan law of the plan the session starts with, before engine/control
    // publishes one: session-format.md's default. A different law in the
    // first published plan would be heard as a ramp in its first block.
    float pan_law_db = -3.0f;
};

// A sample-accurate event for the audio thread, drained at block start
// (ADR-0007 §2). Trivial, so the ring can hold it as bytes.
struct RtCommand {
    // Append-only.
    enum class Kind : uint32_t { None = 0, Locate = 1, PluginParam = 2 };
    Kind kind;
    uint64_t frame;  // Locate
    // PluginParam: the chain's parameter store (the index its plan node
    // carries), the instance in the chain, the dense parameter index or
    // plugin::kBypassParam, and the format-native value (ADR-0012 §5.2).
    uint32_t store;
    uint32_t instance;
    uint32_t param;
    float value;
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
    // Capture boundary records the disk thread may fall behind by. 0: eight
    // per track, at least 64. A full ring is an overrun, never a wait.
    uint32_t boundary_records = 0;
    // How often the disk thread checkpoints every open take: header patched
    // to the frames written, data synced (WavWriter::checkpoint()). With the
    // capture ring's depth this is the stated loss window per track at a
    // power cut (session-format.md, "Crash recovery"). 2 s is the spec's
    // placeholder until the sync's cost on a stick is measured (ADR-0009
    // Unverified 8).
    double sync_seconds = 2.0;
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

    // Opens a file per armed track and records into it, from the start of
    // the audio thread's next block. Stopped, the transport starts rolling;
    // rolling, the take starts where it is (punch in on the fly); during a
    // take, that take ends and this one starts on the same frame, with no
    // frame lost or repeated between them. The take's origin (its files'
    // TimeReference) is the timeline frame of its first recorded sample,
    // which only the audio thread knows, and reaches the files through the
    // disk thread. All or nothing: if any armed track's file cannot be
    // created -- above all because it already exists -- nothing is
    // recorded, the files this call created are removed, and `error`, if
    // given, says which file and why.
    bool start_recording(int take_number, std::string* error = nullptr);
    // Ends the running take and leaves the transport rolling (punch out on
    // the fly). The audio thread ends it at its next block; the disk thread
    // closes its files after that, and closed_takes() reports it. False if
    // no take is running.
    bool punch_out();
    // Stops the transport and closes every open file, waiting for the
    // callback in flight, if any, to finish first. Returns false if any
    // take since the last stop() failed on any track -- the one it ends, or
    // one punched out or started over while rolling; a disk error, or the
    // file reaching the 4 GB WAV limit -- and `error`, if given, says which
    // file and why. The files are closed either way, and one that reached
    // the limit is valid up to its last whole frame. Leaves the disk thread
    // stopped; disk().start() before the next take.
    bool stop(std::string* error = nullptr);

    // A take whose files are all closed, each with the length it has on
    // disk ("take closed, length N").
    struct ClosedTake {
        int take = 0;
        std::vector<DiskThread::ClosedFile> files;
    };
    // The takes closed since the last call, oldest first: by stop(), or by
    // the disk thread after punch_out() or a take started over it. Control
    // thread. With the disk thread not running, drains on this thread.
    std::vector<ClosedTake> closed_takes();
    // The take being recorded, as the control thread last set it; 0 if none.
    int recording_take() const { return recording_take_; }
    // Empty, or which file failed since the last stop() and why, in the
    // words stop() will use: the running take's, or a take's ended while
    // rolling. Writing stops at the failure, so the operator is told now
    // rather than when the transport stops. Control thread; allocates.
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

    // Queues a plugin parameter change for the audio thread (ADR-0012
    // §5.1). At the start of the next callback it is handed, at offset 0
    // and in the order posted, to the InsertChain node whose plan names
    // `store`; that node keeps it in the store and tags it with the block it
    // submits. One the plan cannot take (no bound node names the store, the
    // callback is mixed in pieces) is dropped and counted in
    // plugin_params_dropped(), never held for a later plan: the control
    // thread seeds a new chain's store from the document. False when
    // refused: no store, a value that is not finite, or the queue full.
    // Control thread; never waits.
    bool post_plugin_param(uint32_t store, uint32_t instance, uint32_t param, float value);
    // Changes the audio thread drained and had no node to hand to.
    uint64_t plugin_params_dropped() const {
        return params_dropped_.load(std::memory_order_relaxed);
    }

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
    // Locates the audio thread dropped because a take had already started.
    uint64_t locates_dropped() const { return locates_dropped_.load(std::memory_order_relaxed); }
    // Capture boundary records the audio thread could not push because the
    // disk thread had fallen behind. A lost open delays that track's take
    // by a block and is counted as an overrun on it too; if the take ends
    // before the retry, its file on that track stays open, empty, and
    // closed_takes() holds the take back, until the track's next open or
    // stop(). A lost close ends the file where the next take starts, or at
    // stop().
    uint64_t boundary_overruns() const {
        return boundary_overruns_.load(std::memory_order_relaxed);
    }
    // Recorded samples no file took (DiskThread::dropped_samples()): above
    // all, a callback that outlasted stop()'s 1 s bound.
    uint64_t dropped_samples() const { return disk_->dropped_samples(); }

    // The timeline frame of the running or last take's first recorded
    // sample, or kNoOrigin before the audio thread has recorded one.
    static constexpr uint64_t kNoOrigin = ~uint64_t{0};
    uint64_t take_origin() const { return take_origin_.load(std::memory_order_acquire); }

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
    // The delay-line pool the builder assigns from, and the longest delay
    // one line holds.
    uint32_t delay_lines() const { return delay_lines_; }
    uint32_t max_delay_frames() const { return delay_len_ ? delay_len_ - 1 : 0; }

    // What InsertChain nodes run; with null, a bound chain plays silence
    // and an unbound one passes through (insert_chain_runner.h). Not
    // owned: a runner must outlive every callback that can have loaded it,
    // so replace or clear it while the backend is active only if the old
    // one is kept until a callback has passed. The release store publishes
    // whatever the runner set up before the call.
    void set_insert_chains(InsertChainRunner* runner) {
        chains_.store(runner, std::memory_order_release);
    }
    // Callbacks whose chains did not run because the callback was longer
    // than max_block_frames, or not the runner's block_frames
    // (insert_chain_runner.h).
    uint64_t chain_bypassed_blocks() const {
        return chain_bypassed_.load(std::memory_order_relaxed);
    }
    // Node blocks a bound InsertChain node played as silence because it was
    // not run: no runner installed, or a callback the runner does not take.
    // Never dry: the plan compensates for the chain's latency, so its input
    // would be early. One per node per mixed slice.
    uint64_t unrun_chain_blocks() const {
        return unrun_chain_blocks_.load(std::memory_order_relaxed);
    }
    // Callbacks played silent because the plan failed the audio thread's
    // bounds check when it was adopted. A plan from the builder never does.
    uint64_t invalid_plan_blocks() const {
        return invalid_plan_blocks_.load(std::memory_order_relaxed);
    }

private:
    // The capture windows: boundary records and the frames inside them.
    void capture(const backend::AudioBlock& block, bool recording, uint32_t take,
                 uint64_t playhead) noexcept SONORA_RT;
    // Pulls the disk thread's closed files into closing_.
    void collect_closed();
    // Until the callback running now, if one is, has returned, or 1 s has
    // passed. False on the timeout.
    bool wait_for_callback() const;

    void mix(const PlaybackPlan& plan, const backend::AudioBlock& block, uint32_t offset,
             uint32_t n, bool rolling, bool recording, uint64_t playhead) noexcept SONORA_RT;
    void render_track(const PlaybackPlan& plan, uint32_t plan_track, uint64_t from,
                      uint32_t n, float* const out[2]) noexcept SONORA_RT;
    void render_segment(const PlaybackPlan& plan, const PlanSegment& seg, uint64_t from,
                        uint32_t n, float* const out[2]) noexcept SONORA_RT;
    // Steps 1-2 of the monitor-bus plan into `out`; true when it is stereo.
    bool render_source(const PlaybackPlan& plan, const PlanNode& node,
                       const backend::AudioBlock& block, uint32_t offset, uint32_t n,
                       bool rolling, bool recording, uint64_t from,
                       float* const out[2]) noexcept SONORA_RT;
    // Step 3: the track's ramped gain and pan, from `in` into stereo `out`.
    void apply_fader(uint32_t engine_track, bool stereo, const float* const in[2],
                     float* const out[2], uint32_t n) noexcept SONORA_RT;
    // Whether every index the audio thread follows in `plan` is in bounds:
    // nodes, edges, tracks, segments, scratch buffers. Run once per plan,
    // when the audio thread adopts it.
    bool plan_in_bounds(const PlaybackPlan& plan) const noexcept SONORA_RT;
    // Adds `in` to `out`, `edge.delay_frames` late through its line.
    void sum_edge(const PlaybackPlan& plan, const PlanEdge& edge, const float* const in[2],
                  float* const out[2], uint32_t n) noexcept SONORA_RT;
    float* scratch(uint32_t buffer, uint32_t side) noexcept SONORA_RT {
        return scratch_.get() + (static_cast<std::size_t>(buffer) * 2 + side) * max_block_;
    }

    SessionConfig cfg_;
    std::unique_ptr<Transport> transport_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::unique_ptr<RingBuffer<CaptureBoundary>> boundaries_;
    std::unique_ptr<DiskThread> disk_;

    // Control thread only. A take's files arrive one by one as the disk
    // thread closes them; it is reported once all `expected` have.
    struct Closing {
        std::size_t expected = 0;
        ClosedTake take;
    };
    std::map<int, Closing> closing_;
    int recording_take_ = 0;
    // "path: why" for every file closed failed since the last stop(), in
    // the order closed. A take punched out or started over has left
    // recording_take_, and its failure must still reach take_failure() and
    // stop() (PR #60, code review 3).
    std::vector<std::string> failed_files_;

    // Declared before the reader, so the reader is gone before the plans it
    // serves are freed.
    std::unique_ptr<PlanExchange> plans_;
    std::unique_ptr<DiskReader> reader_;
    int64_t window_frames_ = 0;
    std::unique_ptr<RingBuffer<RtCommand>> commands_;
    std::unique_ptr<RingBuffer<RtCommand>> param_commands_;
    // This callback's parameter changes, drained at its start (one taken by
    // a node is marked None), and the ones gathered for the node being run.
    // Both the ring's capacity.
    std::unique_ptr<RtCommand[]> params_;
    std::unique_ptr<ChainParamEvent[]> node_params_;
    uint32_t params_count_ = 0;

    // Audio-thread state, sized here and never resized.
    // Per track: the take its capture window is open for, or kNoWindow, and
    // the samples it has written to its capture ring, ever.
    static constexpr uint32_t kNoWindow = ~uint32_t{0};
    std::unique_ptr<uint32_t[]> window_take_;
    std::unique_ptr<uint64_t[]> captured_;
    // The take take_origin_ was set for, or kNoWindow.
    uint32_t origin_take_ = kNoWindow;
    uint32_t max_block_ = 0;
    uint32_t scratch_buffers_ = 0;           // the most nodes a plan has
    std::unique_ptr<float[]> scratch_;       // scratch_buffers x 2 x max_block
    std::unique_ptr<float[]> track_buf_;     // 2 x max_block
    // The delay-line pool: channel state outside the plan, so a swap keeps
    // it. Each line remembers the edge it serves and how many frames of it
    // it holds, so a line handed to another edge restarts from silence
    // without being cleared on the audio thread.
    uint32_t delay_lines_ = 0;
    uint32_t delay_len_ = 0;
    std::unique_ptr<float[]> delay_;         // lines x 2 x (max_delay + 1)
    std::unique_ptr<uint32_t[]> delay_pos_;
    std::unique_ptr<uint64_t[]> line_owner_; // edge_pair(); 0 is none
    std::unique_ptr<uint32_t[]> line_filled_;
    // The callback that last wrote each line. A line skipped for even one
    // callback (its edge undelayed or gone) holds a past with a hole in
    // it, and is restarted from silence when its edge comes back.
    std::unique_ptr<uint64_t[]> line_stamp_;
    uint64_t callbacks_ = 0;
    // The last period's start and size, to carry an unstamped period's.
    uint64_t last_callback_ns_ = 0;
    uint32_t last_nframes_ = 0;
    std::atomic<InsertChainRunner*> chains_{nullptr};
    // This callback's runner, when its chains run: loaded once per callback.
    InsertChainRunner* runner_ = nullptr;
    bool plan_valid_ = true;
    std::unique_ptr<uint32_t[]> cursor_;     // per plan track
    // The frame each plan track's cursor is good for: the end of what it
    // last rendered. A track that sent its input, or nothing, has not
    // followed a locate, so it seeks again rather than trust its cursor.
    std::unique_ptr<uint64_t[]> cursor_frame_;
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
    std::atomic<uint64_t> locates_dropped_{0};
    std::atomic<uint64_t> chain_bypassed_{0};
    std::atomic<uint64_t> invalid_plan_blocks_{0};
    std::atomic<uint64_t> boundary_overruns_{0};
    std::atomic<uint64_t> params_dropped_{0};
    std::atomic<uint64_t> unrun_chain_blocks_{0};
    // Reset by start_recording() before it publishes the take; set by the
    // audio thread in the take's first block.
    std::atomic<uint64_t> take_origin_{kNoOrigin};
    // Odd while process() runs; each callback adds one on entry and one on
    // exit. stop() waits, after storing Stopped, for the callback in flight
    // to return, so none that read the take before the store is still
    // writing it when the files are closed. A count, not a flag: any change
    // proves that callback returned, so stop() never has to land in the gap
    // between two callbacks, which near full DSP load is far shorter than
    // its poll (PR #60, RT audit 1).
    std::atomic<uint64_t> callback_seq_{0};
    // process() bumps it on every callback; a lock-based fallback would put
    // a lock on the audio thread.
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "callback_seq_ must be lock-free");
};

}  // namespace sonora
