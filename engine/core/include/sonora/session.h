// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sonora/automation_write.h"
#include "sonora/click.h"
#include "sonora/disk_reader.h"
#include "sonora/disk_thread.h"
#include "sonora/fade_curve.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/latency_probe.h"
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
    // 0: two plans' windows for every track channel, each at the playhead
    // and at a loop's start (DiskReader::hold_loop()), plus a margin. While
    // a new loop range waits for its start the playing plan holds a third,
    // the old start, which the pending plan's share covers when no edit is
    // pending; with one, the change waits for the swap.
    uint32_t pool_chunks = 0;
    uint32_t max_sources = 1024;
    uint32_t max_segments = 65536;          // per plan
    uint32_t max_chunk_slots = 1u << 20;    // per plan
    // A larger block is mixed in pieces of this size.
    uint32_t max_block_frames = 1024;
    // Plugin delay compensation (ADR-0012 §7.3): the most one edge is
    // delayed, and the most edges one plan delays (auditor L3). 0 frames
    // allocates no delay line. The pool is never larger than the most edges
    // a plan of this graph can delay: one per track and per bus return into
    // the bus, and one per send. Not sized for every send: at 16384 frames a
    // line is 128 KiB, and 18 tracks with 32 buses could ask for 550. The
    // bus's own edges are served first (compensate()), and 18 + 32 fit in
    // 128 with lines to spare for the sends; a send left without one plays
    // early and is counted in undelayed_edges (test_sends).
    uint32_t max_delay_frames = 16384;
    uint32_t max_delayed_edges = 128;
    // Aux buses a plan may carry (ADR-0022 §8). Sizes the scratch buffers
    // and the send and return state; a plan with more is refused.
    uint32_t max_buses = 32;
    uint32_t command_capacity = 64;
    // Plugin parameter changes queued for the audio thread (ADR-0012 §5.1),
    // in a ring of their own so a knob drag never holds a locate back. The
    // ring is drained every callback, so this bounds what can be posted in
    // one period, across every chain.
    uint32_t plugin_param_capacity = 256;
    // Parameter stores a plan may name: engine/plugin_client's
    // ClientConfig::max_stores. Sizes the store-to-node index the audio
    // thread builds when it adopts a plan (#68 RT audit N1).
    uint32_t max_param_stores = 256;
    // Punched takes started (start_punch()) faster than the audio thread
    // drains them: one more is refused, never waited on.
    uint32_t punch_capacity = 8;
    // How long a change of a track's monitor source takes (input to
    // playback and back, a mode switched, a take started or punched out on
    // the fly): a ramp from the frame it takes effect, never a step at a
    // block boundary, which clicks. A punch's own crossfade replaces it at
    // the punch points (PunchRange). 0 switches at once.
    uint32_t monitor_ramp_frames = 64;
    // The pan law of the plan the session starts with, before engine/control
    // publishes one: session-format.md's default. A different law in the
    // first published plan would be heard as a ramp in its first block.
    float pan_law_db = -3.0f;
};

// A sample-accurate event for the audio thread, drained at block start
// (ADR-0007 §2). Trivial, so the ring can hold it as bytes.
struct RtCommand {
    // Append-only.
    enum class Kind : uint32_t { None = 0, Locate = 1, PluginParam = 2, Loop = 3 };
    Kind kind;
    uint64_t frame;  // Locate; Loop: A
    uint64_t to;     // Loop: B
    // PluginParam: the chain's parameter store (the index its plan node
    // carries), the instance in the chain, the dense parameter index or
    // plugin::kBypassParam, and the format-native value (ADR-0012 §5.2).
    // `epoch`: the plan whose `store` the change was posted for (PR #77 RT
    // audit M1).
    uint64_t epoch;
    uint32_t store;
    uint32_t instance;
    uint32_t param;
    float value;
};

// Where a take records and where Auto monitoring hears it, in timeline
// frames, sample-accurate inside a block (timeline-model.md, "Punch in"):
// capture over [capture_from, capture_to), the punch range [A, B) plus its
// handles; an armed Auto track switches from playback to its input at A
// (monitor_in) and back at B (monitor_out), each a crossfade of `crossfade`
// frames and `shape`, centred on the point: the crossfade the take will be
// placed with, so what is heard while punching is what plays back after.
// A crossfade of 0 is never a cut: it gets PlaybackConfig::
// monitor_ramp_frames, centred on each point the same way.
struct PunchRange {
    static constexpr uint64_t kOpenEnd = ~uint64_t{0};
    uint64_t capture_from = 0;
    uint64_t capture_to = kOpenEnd;
    uint64_t monitor_in = 0;
    uint64_t monitor_out = kOpenEnd;
    uint32_t crossfade = 0;
    FadeCurve shape = FadeCurve::Linear;
    // Pre-roll: where a take started with the transport stopped is located
    // first, A less the pre-roll. engine/control's to act on; the session
    // does not locate.
    uint64_t start = 0;
    // Post-roll: the roll end start_punch() sets (set_roll_end()), B plus
    // the post-roll. kOpenEnd rolls on after the punch-out.
    uint64_t roll_end = kOpenEnd;
};

// A loop take (ADR-0016 §1, §2): it records from `start`, A less the
// pre-roll, through every wrap of the playhead from B back to A, until it
// is stopped. In timeline frames.
struct LoopRange {
    uint64_t from = 0;   // A
    uint64_t to = 0;     // B
    uint64_t start = 0;  // at most A
    // The input latency L in frames: a wrap's boundary record counts the
    // capture to the sample that lines up with A, which arrives L frames
    // after the playhead wraps (C7). The session's input_latency() (ADR-0019).
    uint32_t latency = 0;
    // The pass handle H: B - A shorter than this is refused (C4).
    uint32_t handle = 0;
};

// A take file's bext Description (session-format.md, "BWF metadata, both
// ways"): "sonora take=<n> track=<id> punch=<A>-<B> id=<session id>
// session=<name>", with " track=<id>" left out when `track_id` is 0,
// " punch=<A>-<B>", the monitored range, only for a punch, and " id=..."
// only when `session_id` is not empty. A loop take says " loop=<A>-<B>" in
// its place (ADR-0016 §4). The name is last and is the rest of
// the field, because a name may hold spaces and "=": "Demo punch=0-48000"
// must not read as a punch. At most 255 bytes, so the field keeps its NUL;
// a name that does not fit is cut on a UTF-8 character boundary.
std::string take_description(int take, uint64_t track_id, const std::string& session,
                             const PunchRange* punch, const LoopRange* loop = nullptr,
                             const std::string& session_id = {}, uint32_t latency = 0);
// The loop range a Description written by take_description() gives, read
// from before its " session=", so a session name cannot fake one. False
// when it is not a loop take's.
bool loop_of_description(const std::string& description, uint64_t& from, uint64_t& to);

// Everything a Description written by take_description() says: what places
// a take recovery finds unplaced (session-format.md, "Crash recovery").
struct TakeDescription {
    int take = 0;
    uint64_t track_id = 0;   // 0 when the field is left out
    bool punched = false;
    bool looped = false;
    uint64_t from = 0;       // the punch or loop range [from, to)
    uint64_t to = 0;
    // The round-trip L its file was captured with (ADR-0019), so recovery
    // can say the file is already compensated; 0 when the field is left
    // out, as it is for L = 0 and for a take written before it.
    uint32_t latency = 0;
    std::string session_id;  // empty for a take written before session ids
    std::string session;
};
// Fields are read from before " session=", one word at a time, and the
// name is the rest. False, `out` unspecified, for anything
// take_description() could not have written: another program's text, a
// field it does not write, an empty range, both a punch and a loop.
bool parse_take_description(const std::string& description, TakeDescription& out);

struct SessionConfig {
    std::string name = "Untitled";
    // The folder the session starts in; Session::folder() is the one in use.
    std::string dir;
    uint32_t sample_rate = 48000;
    int track_count = 8;
    uint16_t channels_per_track = 1;
    WavWriter::Format format = WavWriter::Format::Int24;
    // Seconds of headroom in each capture ring before an overrun is possible.
    double ring_seconds = 4.0;
    // Capture boundary records the disk thread may fall behind by. 0: eight
    // per track, at least 64, plus the Wrap records a loop take pushes
    // while the disk thread stalls for disk_stall_seconds. A full ring is an
    // overrun, never a wait.
    uint32_t boundary_records = 0;
    // The longest the disk thread is assumed to stop draining (one sync on
    // a slow stick). The shortest loop is one max_block_frames block, so a
    // loop take pushes at most one Wrap per track per block: the ring holds
    // that many for this long. 0.5 s is a placeholder until ADR-0009
    // Unverified 8 measures the sync.
    double disk_stall_seconds = 0.5;
    // How often the disk thread checkpoints every open take: header patched
    // to the frames written, data synced (WavWriter::checkpoint()). With the
    // capture ring's depth this is the stated loss window per track at a
    // power cut (session-format.md, "Crash recovery"). 2 s is the spec's
    // placeholder until the sync's cost on a stick is measured (ADR-0009
    // Unverified 8).
    double sync_seconds = 2.0;
    // The passes a loop take places (ADR-0016 O2). The take records on past
    // it; the wrap list each closed file carries is sized to it here, and
    // the wraps after it are on disk only.
    uint32_t loop_pass_cap = 99;
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
    // start_recording(), recording only inside `range`: the take's files
    // are opened now, when the punch is armed, and the audio thread opens
    // and closes each track's capture window at the range's frames. Past
    // capture_to the files close while the transport rolls on, still
    // Recording, until follow_punch_out(), punch_out() or stop() ends the
    // take; closed_takes() reports it then. A range wholly behind the
    // take's first block closes the files at once, empty. Refused, with nothing opened, when the range is empty,
    // when its monitored part [monitor_in, monitor_out) is not inside what
    // it captures, or when that monitored part ends at or before the
    // playhead: past monitor_out only handle is left, which is placed
    // nowhere (add_closed_take()). A capture_from
    // already passed records from the audio thread's next block.
    bool start_punch(int take_number, const PunchRange& range, std::string* error = nullptr);
    // start_recording(), as a loop take (ADR-0016): the take records from
    // loop.start, located there first wherever the playhead is, and the
    // audio thread wraps the playhead from B to A until stop(), marking
    // each wrap in band on every armed track. Each file gets a wrap list
    // next to it (WrapSidecar). Refused, with nothing opened, during a
    // take, for an empty range, a start after A, or a loop shorter than
    // its handle or than one block (PlaybackConfig::max_block_frames), so
    // no callback within that bound holds two wraps.
    bool start_loop(int take_number, const LoopRange& loop, std::string* error = nullptr);
    // Ends the running take and leaves the transport rolling (punch out on
    // the fly). The audio thread ends it at its next block; the disk thread
    // closes its files after that, and closed_takes() reports it. False if
    // no take is running.
    bool punch_out();
    // Ends the running punched take as punch_out() does once the audio
    // thread has recorded it up to capture_to, or found its range already
    // behind its first block: its files are closed in band, and a transport
    // left in Recording would show a take while nothing records (Pro Tools'
    // record button drops out at the punch-out). True if it ended it.
    // Control thread, every turn.
    bool follow_punch_out();
    // Stops the transport and closes every open file, waiting for the
    // callback in flight, if any, to finish first. Returns false if the
    // newest take failed on any track -- the one it ends, or the one last
    // punched out while rolling; a disk error, or the file reaching the
    // 4 GB WAV limit -- and `error`, if given, says which file and why. An
    // earlier take's failure is not repeated: closed_takes() reported it,
    // and a later take started since. The files are closed either way, and one that reached
    // the limit is valid up to its last whole frame. Leaves the disk thread
    // stopped; disk().start() before the next take.
    bool stop(std::string* error = nullptr);

    // A take whose files are all closed, each with the length it has on
    // disk ("take closed, length N").
    struct ClosedTake {
        int take = 0;
        std::vector<DiskThread::ClosedFile> files;
        // Started by start_punch(), with this range: it is placed over
        // [monitor_in, monitor_out) only, not over all it captured.
        bool punched = false;
        PunchRange punch;
        // Started by start_loop(), with this range; each file's wraps are
        // in DiskThread::ClosedFile::wraps.
        bool looped = false;
        LoopRange loop;
        // The wraps the audio thread made in it, whatever reached the files:
        // a file's list shorter than this, or holding a lost wrap, lost
        // records to a full boundary ring (ADR-0016 §5).
        uint32_t loop_wraps = 0;
        // The frame its first block recorded from (take_origin()), or
        // kNoOrigin if it ended before the audio thread ran a block of it.
        // With no frame in any file, what says why nothing was captured.
        uint64_t origin = kNoOrigin;
        // The playhead when the control thread ended it, read before the
        // transport left Recording, so no block it counts went unrecorded:
        // past capture_from with no frame captured is an overrun, not an
        // early stop. kNoOrigin until it ends.
        uint64_t end = kNoOrigin;
        // The round trip its files were compensated by (ADR-0019 §2): a
        // plain or punched take captured this much later on the playhead
        // than the timeline frames its files are labelled with. A loop
        // take's is loop.latency, applied to its wraps instead, and this
        // says the same.
        uint32_t latency = 0;
    };
    // The takes closed since the last call, oldest first: by stop(), or by
    // the disk thread after punch_out() or a take started over it. Control
    // thread. With the disk thread not running, drains on this thread.
    std::vector<ClosedTake> closed_takes();
    // The take being recorded, as the control thread last set it; 0 if none.
    int recording_take() const { return recording_take_; }
    // Whether any take's files are still open or not yet collected by
    // closed_takes(): the running take, and one punched out or started
    // over whose close the disk thread has not reported. Control thread.
    bool takes_closing() const { return !closing_.empty(); }
    // A take other than the running one whose files are not all in yet (a
    // punch-out, a take started over): its number, or 0. Its files are
    // placed on the tracks by row, so nothing may move until it is in
    // (ADR-0023 §5). Control thread.
    int closing_take() const;
    // Whether that take was started by start_loop(). Control thread.
    bool loop_take_running() const { return recording_take_ != 0 && loop_running_; }
    // Empty, or which file of the newest take failed and why, in the words
    // stop() will use: the running take's, or, after a punch-out, the take
    // just ended, until the next take starts (ADR-0007: the next clean take
    // clears the alert). Writing stops at the failure, so the operator is
    // told now rather than when the transport stops. Control thread;
    // allocates.
    std::string take_failure() const;

    // Transport while not recording. play() rolls from the playhead;
    // stop_playback() stops a roll and leaves a take alone (stop() ends
    // one). Control thread.
    void play();
    void stop_playback();

    // Post-roll: a roll stops at `frame`. The audio thread plays the frames
    // before it, silences the rest of that block's playback, leaves the
    // playhead on it (or where it is, if already past) and stops the
    // transport, then clears the end. Only a roll: a take is never stopped
    // by it, and once its punch-out leaves the transport rolling past the
    // end, the next block stops it. Ignored while a loop wraps (ADR-0016:
    // post-roll does not apply while looping). start_punch() sets the
    // range's roll_end; start_recording() and start_loop() clear it.
    // Control thread.
    static constexpr uint64_t kNoRollEnd = ~uint64_t{0};
    void set_roll_end(uint64_t frame) { roll_end_.store(frame, std::memory_order_release); }
    uint64_t roll_end() const { return roll_end_.load(std::memory_order_acquire); }

    // The round trip L in frames, from a sample the callback writes to the
    // sample it reads back (ADR-0019): the input sample that lines up with
    // timeline frame t arrives at playhead t + L. A plain or punched take
    // captures its timeline range L frames later on the playhead and labels
    // its files with the frames they line up with, so its TimeReference is
    // P0 - L (session-format.md); a loop take counts its wraps with it
    // (ADR-0016 §2). Monitoring is not moved by it. The audio thread reads
    // it at a take's first block. Refused, changing nothing, during a take
    // and above one second. Control thread.
    bool set_input_latency(uint32_t frames, std::string* error = nullptr);
    uint32_t input_latency() const { return input_latency_.load(std::memory_order_relaxed); }
    // Measures L through a cable from an output to an input; the session
    // runs it at the end of every callback (latency_probe.h). The result is
    // not applied: the caller decides, and set_input_latency() does it.
    LatencyProbe& latency_probe() { return *probe_; }
    // The click and the count-off (ADR-0020). A take started with the
    // transport stopped counts off first while Click::count_off() is on.
    Click& click() { return *click_; }

    // Queues a locate for the audio thread, which applies it at the start of
    // a block. False if the queue is full. engine/control sends it only once
    // the target's window is resident (ADR-0009 §4.4).
    bool post_locate(uint64_t frame);

    // The playback loop (engine.transport.loop): rolling and not recording,
    // a block that starts before B wraps the playhead from B to A at the
    // sample; one that starts at or past B plays on. [0, 0) clears it. The
    // range reaches the audio thread only once the reader has its start
    // resident (pump_loop(), ADR-0009 §4.4); until then the range sent last
    // plays on, still held. Refused, leaving the loop as it was, for a range
    // shorter than one block (PlaybackConfig::max_block_frames), so no
    // callback within that bound wraps twice. A take is one contiguous span
    // and does not wrap at it; a loop take wraps at its own range. A range
    // set during any take is held and sent when it ends. Control thread.
    bool set_loop(uint64_t from, uint64_t to, std::string* error = nullptr);
    // Sends the range set_loop() set last once its start is resident; a
    // full queue leaves it for the next call. Control thread, every turn.
    void pump_loop();
    // Whether that range is not sent yet.
    bool loop_pending() const { return !(loop_wanted_ == loop_sent_); }

    // Queues a plugin parameter change for the audio thread (ADR-0012
    // §5.1), for the plan published with `epoch`, whose node names `store`.
    // At the start of the next callback that runs chains under that plan it
    // is handed, at offset 0 and in the order posted, to that InsertChain
    // node, which keeps it in the store and tags it with the block it
    // submits. A callback that runs no chain (zero frames, another size,
    // mixed in pieces, no chain in the plan, no runner) leaves it queued,
    // and so does one still on an earlier plan than `epoch` (published,
    // not yet adopted). Dropped and counted in plugin_params_dropped(),
    // never handed to another plan's node: one posted for a plan the audio
    // thread has since replaced, whose store may now be another chain's
    // (PR #77 RT audit M1); and one no runnable node of its plan takes (no
    // bound node names the store, or it is silent). The control thread
    // seeds a new chain's store from the document, and re-posts or reseeds
    // when the count moves. False when refused: epoch 0 (no plan), no
    // store, a store past PlaybackConfig::max_param_stores, a value that is
    // not finite, or the queue full. Control thread; never waits.
    bool post_plugin_param(uint64_t epoch, uint32_t store, uint32_t instance, uint32_t param, float value);
    // Changes the audio thread drained and had no node of their plan to
    // hand to.
    uint64_t plugin_params_dropped() const {
        return params_dropped_.load(std::memory_order_relaxed);
    }

    // Sends and aux returns (ADR-0022 §6): mix state, read once per block
    // and ramped across it like a fader. A send's state is at strip *
    // kStripSendSlots + slot: strip t is engine track t, strip tracks + b
    // is engine bus b. Linear gains; pan -1 to 1. Out of range is ignored.
    // Control thread.
    void set_send(uint32_t strip, uint32_t slot, float level, float pan, bool follow_pan,
                  bool mute, bool pre);
    // `solo` solos what feeds the bus as well (ADR-0024 §4); the return itself
    // is never muted by a solo.
    void set_aux(uint32_t bus, float gain, float pan, bool mute, bool solo = false);
    // A solo left on a bus slot the document no longer has would mute the
    // rest while every replica shows none, as a track's does.
    void set_aux_solo(uint32_t bus, bool solo);
    // A fader ride on a send or an aux return (ADR-0022 update of
    // 2026-10-31): the level or gain alone, the rest of the strip's state
    // left as the document set it. Out of range is ignored. Control thread.
    void set_send_level(uint32_t strip, uint32_t slot, float level);
    void set_aux_gain(uint32_t bus, float gain);
    // What the audio thread reads next; 0 out of range. Any thread.
    float send_level(uint32_t strip, uint32_t slot) const;
    float aux_gain(uint32_t bus) const;
    uint32_t max_buses() const { return max_buses_; }

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
    // Punched takes the audio thread held no range for, which recorded
    // nothing. Zero by construction; counted so a broken bound is seen.
    uint64_t punches_lost() const { return punches_lost_.load(std::memory_order_relaxed); }

    // Live automation writes (ADR-0027 §3): each block a written parameter
    // plays its live value while rolling, the audio thread stamps what it
    // applied. Control thread: up to `max` writes, oldest first.
    std::size_t take_automation_writes(AutomationWrite* out, std::size_t max) {
        return auto_writes_->read(out, max);
    }
    // The last pass whose roll has ended: once this reaches a pass and the
    // writes are drained after reading it, the pass has every write it had.
    uint64_t automation_passes_ended() const {
        return auto_passes_ended_.load(std::memory_order_acquire);
    }
    // The pass of the roll now playing, or of the last one; 0 before any.
    uint64_t automation_pass() const { return auto_pass_now_.load(std::memory_order_acquire); }
    // A Load while rolling: from the next callback no track plays a Latch
    // touched before it. Clear the touched flags first, or one still held
    // latches again.
    void forget_latches() { auto_unlatch_.fetch_add(1, std::memory_order_release); }
    // Writes the audio thread dropped on a full ring: a gap in a pass.
    uint64_t automation_write_overruns() const {
        return auto_write_overruns_.load(std::memory_order_relaxed);
    }
    // How long a released Touch parameter takes to return to its lane: Pro
    // Tools' AutoMatch default, 250 ms, until the session carries its own.
    uint32_t touch_return_frames() const { return touch_return_frames_; }

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
    // The active session folder (ADR-0018 §1): takes, their wrap lists and
    // next_free_take() resolve against it, and engine/control reads it for
    // sources, plugin states and flushes. SessionConfig::dir until
    // set_folder(). Shared, never a reference to a member: a reader keeps
    // the folder it was given. Control thread.
    std::shared_ptr<const std::string> folder() const { return folder_; }
    // Empty, or why the active folder may not change now: a take is open,
    // or one is closing (a file the disk thread holds, or a take
    // closed_takes() has not yet collected). A take closes into the folder
    // it was opened in. Control thread.
    std::string folder_change_refusal() const;
    // Makes `path` the active folder, unless folder_change_refusal() says
    // why not. Locks, creates and recovers nothing: engine/control's
    // ActiveFolder::enter() does, and then calls this. Control thread.
    bool set_folder(std::string path, std::string& why);
    // The name a take's files give in their Description: config().name until
    // set, then the document's (PlaybackController::show()). Control thread.
    const std::string& session_name() const { return session_name_; }
    void set_session_name(std::string name) { session_name_ = std::move(name); }
    // The document's session id, which each take's Description carries
    // from the next take on. Control thread.
    const std::string& session_id() const { return session_id_; }
    void set_session_id(std::string id) { session_id_ = std::move(id); }
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
    // would be early. Faded by the runner's skip() when there is a runner.
    // One per node per mixed slice.
    uint64_t unrun_chain_blocks() const {
        return unrun_chain_blocks_.load(std::memory_order_relaxed);
    }
    // Node blocks played silent because the plan lays the chain out at
    // kSilentChannelSlot (down, or starting with the session). One per node
    // per mixed slice.
    uint64_t silent_chain_blocks() const {
        return silent_chain_blocks_.load(std::memory_order_relaxed);
    }
    // Callbacks played silent because the plan failed the audio thread's
    // bounds check when it was adopted. A plan from the builder never does.
    uint64_t invalid_plan_blocks() const {
        return invalid_plan_blocks_.load(std::memory_order_relaxed);
    }

private:
    // Only a test defines it, to reach word_hook_.
    friend struct SessionTestAccess;
    // Called by process() right after it has loaded the transport and
    // before it drains anything, so a test can land the control thread's
    // calls exactly there. Set only while no callback runs.
    using WordHook = void (*)(void* context) noexcept SONORA_RT;
    WordHook word_hook_ = nullptr;
    void* word_hook_context_ = nullptr;

    // No take: a track with no capture window, a punch record for none.
    static constexpr uint32_t kNoWindowTake = ~uint32_t{0};
    // A take's PunchRange as it travels to the audio thread, tagged with
    // its take. Trivial, so the ring can hold it as bytes. A loop take's
    // captures and hears all of the take, and wraps over [loop_from,
    // loop_to) when loop_to > loop_from.
    struct PunchRecord {
        uint32_t take;
        uint32_t crossfade;
        FadeCurve shape;
        uint64_t capture_from;
        uint64_t capture_to;
        uint64_t monitor_in;
        uint64_t monitor_out;
        uint64_t loop_from;
        uint64_t loop_to;
        uint32_t latency;
    };
    // What a take started without a range records and hears: all of it.
    static constexpr PunchRecord kWholeTake = {kNoWindowTake, 0, FadeCurve::Linear, 0,
                                               PunchRange::kOpenEnd, 0, PunchRange::kOpenEnd,
                                               0, 0, 0};
    // A punched take whose range the audio thread does not hold, which the
    // table's sizing rules out: it records nothing and hears playback,
    // rather than recording outside a range it cannot see.
    static constexpr PunchRecord kNoCapture = {kNoWindowTake,        0, FadeCurve::Linear,
                                               PunchRange::kOpenEnd, PunchRange::kOpenEnd,
                                               PunchRange::kOpenEnd, PunchRange::kOpenEnd,
                                               0, 0, 0};

    // start_recording(), start_punch() and start_loop(); `range` and
    // `loop` null for a whole take.
    bool begin_take(int take_number, const PunchRange* range, const LoopRange* loop,
                    std::string* error);
    // The timeline frame `off` frames into a block that starts at
    // `playhead`, with this block's loop applied.
    uint64_t loop_frame(uint64_t playhead, uint64_t off) const noexcept SONORA_RT;
    // render_track() for a span the timeline does not wrap inside.
    void render_span(const PlaybackPlan& plan, uint32_t plan_track, uint64_t from, uint32_t n,
                     float* const out[2]) noexcept SONORA_RT;
    // The capture windows: boundary records and the frames inside them,
    // opened and closed at punch_'s frames.
    // `first`: the take's first block.
    void capture(const backend::AudioBlock& block, bool recording, uint32_t take,
                 uint32_t start, uint64_t playhead, bool first) noexcept SONORA_RT;
    // The running take's origin and `end` into its closing_ entry, before
    // recording_take_ moves on.
    void keep_origin(uint64_t end);
    // Pulls the disk thread's closed files into closing_.
    void collect_closed();
    // The playback loop into the reader: the range set last, with the one
    // the audio thread plays until that is sent. Not during any take: a
    // loop take's own range is held, and a plain one records through B.
    void hold_playback_loop();
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
    // Engine track e's mix as this block hears it: the plan's, while the
    // track's atomics still hold the track order before the plan's
    // (ADR-0023 §4), and the atomics' otherwise.
    struct TrackMixView {
        float gain = 1.0f;
        float pan = 0.0f;
        bool mute = false;
        bool solo = false;
        bool solo_safe = false;
        bool armed = false;
        Track::Monitor monitor = Track::Monitor::Auto;
        uint32_t input = 0;
        Track::Automation automation = Track::Automation::Read;
    };
    TrackMixView mix_view(const PlaybackPlan& plan, std::size_t e) const noexcept SONORA_RT;
    // Zeroes what render_track() put in `out` at or past play_until_.
    void cut_at_roll_end(float* const out[2], uint32_t offset, uint32_t n) const noexcept SONORA_RT;
    // Step 3: the track's ramped gain and pan, from `in` into stereo `out`.
    void apply_fader(uint32_t engine_track, bool stereo, const float* const in[2],
                     float* const out[2], uint32_t n) noexcept SONORA_RT;
    // The fader of a track whose lanes play this callback (ADR-0027 §2):
    // each frame's coefficient from the lanes at its timeline frame, `from`
    // for the first, `lag` frames earlier for a fader behind an insert
    // chain. Falls to apply_fader() for a track that is not automated.
    void apply_track_fader(const PlaybackPlan& plan, const PlanTrack& pt, bool stereo,
                           const float* const in[2], float* const out[2], uint32_t n,
                           uint64_t from, uint32_t lag) noexcept SONORA_RT;
    // The coefficients of engine track e at timeline frame t, L and R,
    // before mute and solo.
    void auto_gains(const PlaybackPlan& plan, const PlanTrack& pt, uint32_t e, int64_t t,
                    float& l, float& r) const noexcept SONORA_RT;
    // Each engine track's automated parameters this callback, and whether
    // its fader crossfades from what it last applied (ADR-0027 §4).
    void automation_targets(const PlaybackPlan& plan, bool rolling,
                            uint64_t playhead) noexcept SONORA_RT;
    // Parameter p's lane on engine track e at frame t, in the lane's unit,
    // leaning toward the value a released Touch parameter left while it
    // returns (ADR-0027 §3).
    float lane_param(const PlaybackPlan& plan, const PlanTrack& pt, uint32_t e, uint32_t p,
                     int64_t t) const noexcept SONORA_RT;
    // Stamps the live value of each parameter in `bits` over the frames this
    // callback plays from `start`, one write per span between wraps.
    void stamp_writes(uint32_t e, uint8_t bits, uint64_t start) noexcept SONORA_RT;
    void push_write(const AutomationWrite& w) noexcept SONORA_RT;
    // Whether every index the audio thread follows in `plan` is in bounds:
    // nodes, edges, tracks, segments, scratch buffers. Run once per plan,
    // when the audio thread adopts it.
    // And whether no engine track is the source of two nodes: a second one
    // would move the track's monitor position twice a frame (PR #81
    // audit, N5).
    bool plan_in_bounds(const PlaybackPlan& plan) noexcept SONORA_RT;
    // The table slot of take `take`'s newest range, or punch_slots_.
    uint32_t newest_punch(uint32_t take) const noexcept SONORA_RT;
    // Adds `in` to `out`, `edge.delay_frames` late through its line.
    void sum_edge(const PlaybackPlan& plan, const PlanEdge& edge, const float* const in[2],
                  float* const out[2], uint32_t n) noexcept SONORA_RT;
    // A send edge: `in` scaled by the send's ramped coefficient, then
    // sum_edge(). `mono`: the source's side 0 feeds both sides.
    void sum_send(const PlaybackPlan& plan, const PlanEdge& edge, bool mono,
                  const float* const in[2], float* const out[2], uint32_t n) noexcept SONORA_RT;
    // This block's send and return targets, from the mix state (ADR-0022
    // §6). A send or return the last callback did not play starts from
    // silence.
    void send_targets(const PlaybackPlan& plan) noexcept SONORA_RT;
    float* scratch(uint32_t buffer, uint32_t side) noexcept SONORA_RT {
        return scratch_.get() + (static_cast<std::size_t>(buffer) * 2 + side) * max_block_;
    }

    SessionConfig cfg_;
    std::shared_ptr<const std::string> folder_;
    std::string session_name_;
    std::string session_id_;
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
    bool loop_running_ = false;
    // The playback loop: set by set_loop(), and last sent to the audio
    // thread, which has taken it once it has applied loops_sent_ ranges.
    // loop_gen_ is the reader generation that holds loop_wanted_.
    LoopHold loop_wanted_;
    LoopHold loop_sent_;
    uint64_t loop_gen_ = 0;
    uint64_t loops_sent_ = 0;
    // The take take_failure() and stop() speak for: the last one started,
    // kept after a punch-out (PR #60, code review 3) and replaced by the
    // next, so take K's failure is never said as take K + 1's (review 4).
    int newest_take_ = 0;
    // Every file closed failed since the last stop(), in the order closed.
    struct FailedFile {
        int take = 0;
        std::string what;  // "path: why"
    };
    std::vector<FailedFile> failed_files_;

    // Declared before the reader, so the reader is gone before the plans it
    // serves are freed.
    std::unique_ptr<PlanExchange> plans_;
    std::unique_ptr<DiskReader> reader_;
    int64_t window_frames_ = 0;
    std::unique_ptr<RingBuffer<RtCommand>> commands_;
    std::unique_ptr<RingBuffer<RtCommand>> param_commands_;
    // This callback's parameter changes as drained, and the same sorted by
    // node: one counting pass by the store-to-node index the audio thread
    // builds when it adopts a plan (#68 RT audit N1), so the gather costs
    // O(ring + nodes) and keeps the posted order within each node. Both
    // the ring's capacity.
    std::unique_ptr<RtCommand[]> params_;
    std::unique_ptr<ChainParamEvent[]> node_params_;
    uint32_t params_count_ = 0;
    // At the front of params_: changes for a plan published but not yet
    // adopted, kept for the callback that adopts it.
    uint32_t params_held_ = 0;
    uint32_t max_param_stores_ = 0;
    std::unique_ptr<uint32_t[]> store_order_;   // max_param_stores_: chain ordinal or kNoNode
    std::unique_ptr<uint32_t[]> node_order_;    // scratch_buffers_: chain ordinal or kNoNode
    std::unique_ptr<uint32_t[]> order_store_;   // per ordinal: its store, to clear at adoption
    std::unique_ptr<uint32_t[]> order_first_;   // per ordinal: first event in node_params_
    std::unique_ptr<uint32_t[]> order_count_;   // per ordinal: events this callback
    std::unique_ptr<uint32_t[]> order_pos_;     // per ordinal: scatter cursor
    uint32_t order_chains_ = 0;                 // ordinals in the adopted plan
    uint32_t max_order_ = 0;                    // the most a plan can have: tracks
    void adopt_param_order(const PlaybackPlan& plan) noexcept SONORA_RT;
    void gather_params() noexcept SONORA_RT;

    // Punch ranges, posted by begin_take() before the take is published;
    // drained by the audio thread at block start into punch_table_.
    std::unique_ptr<RingBuffer<PunchRecord>> punches_;
    // Punched takes the audio thread found no range for. Zero unless the
    // table's sizing is wrong.
    std::atomic<uint64_t> punches_lost_{0};

    // Audio-thread state, sized here and never resized.
    // Per track: the take its capture window is open for, or kNoWindow, and
    // the samples it has written to its capture ring, ever.
    static constexpr uint32_t kNoWindow = kNoWindowTake;
    std::unique_ptr<uint32_t[]> window_take_;
    // Per track: the Transport::Word::start of the take its window is for.
    std::unique_ptr<uint32_t[]> window_start_;
    std::unique_ptr<uint64_t[]> captured_;
    // The take take_origin_ was set for, or kNoWindow.
    uint32_t origin_take_ = kNoWindow;
    // Transport::Word::start of the take the callback last saw recording.
    uint32_t recording_start_ = 0;
    // The ranges drained so far, newest stored last (punch_stored_). One
    // more entry than the ring holds, and the running take's never
    // overwritten: a drain takes at most the ring's capacity, so every
    // record drained while the take the block plays is still the word's
    // has a slot of its own (PR #81 review, N1). A take numbered again
    // after its files were removed finds its newest record.
    std::unique_ptr<PunchRecord[]> punch_table_;
    std::unique_ptr<uint64_t[]> punch_stored_;
    uint32_t punch_slots_ = 0;
    uint32_t punch_next_ = 0;
    uint64_t punch_count_ = 0;
    // This block's: the running take's range, kWholeTake, or kNoCapture.
    const PunchRecord* punch_ = &kWholeTake;
    // The running take's L, latched from input_latency_ at its first block;
    // 0 for a loop take, whose wraps carry L instead. Its capture range on
    // the playhead: the timeline range plus that, saturating at kOpenEnd.
    uint64_t take_latency_ = 0;
    uint64_t capture_from_ = 0;
    uint64_t capture_to_ = PunchRange::kOpenEnd;
    // This block's loop, from punch_: [0, 0) when the playhead does not
    // wrap. The running loop take's wraps so far, and this block's: how
    // many, and the first one's offset in the block.
    uint64_t loop_from_ = 0;
    uint64_t loop_to_ = 0;
    uint32_t loop_wrap_count_ = 0;
    uint32_t block_wraps_ = 0;
    uint32_t block_wrap_offset_ = 0;
    // The playback loop as the last Loop command set it; [0, 0) for none.
    uint64_t play_loop_from_ = 0;
    uint64_t play_loop_to_ = 0;
    // Per engine track, where its monitor source stands, 0 playback and 1
    // input, and the callback that last moved it. A track not rendered in
    // the callback before starts where its source is, with no ramp: it has
    // no sound to ramp from.
    std::unique_ptr<float[]> monitor_pos_;
    std::unique_ptr<uint64_t[]> monitor_stamp_;
    // Per engine track, the plan_in_bounds() pass that last saw a source
    // node for it.
    std::unique_ptr<uint64_t[]> source_seen_;
    uint64_t bounds_pass_ = 0;
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
    // The runner loaded this callback whether or not it runs the chains:
    // its skip() fades a node not run.
    InsertChainRunner* skip_runner_ = nullptr;
    bool plan_valid_ = true;
    std::unique_ptr<uint32_t[]> cursor_;     // per plan track
    // The frame each plan track's cursor is good for: the end of what it
    // last rendered. A track that sent its input, or nothing, has not
    // followed a locate, so it seeks again rather than trust its cursor.
    std::unique_ptr<uint64_t[]> cursor_frame_;
    std::unique_ptr<float[]> track_gain_;    // per engine track, L and R, end of last block
    std::unique_ptr<float[]> track_target_;  // this block's targets
    // Per engine track this block: 0 when muted or solo-muted, else 1; the
    // fader's linear gain; the pan. What its sends' coefficients read.
    std::unique_ptr<float[]> track_open_;
    std::unique_ptr<float[]> track_level_;
    std::unique_ptr<float[]> track_pan_;
    // Automation (ADR-0027 §2), per engine track. Bit kAutoVolume and bit
    // kAutoPan: the parameter plays its lane this callback, and last
    // callback; 1 when the fader crossfades this callback; the open state
    // last callback; the static gain and pan an unautomated parameter keeps,
    // which until automation_targets() rewrites them are last callback's.
    std::unique_ptr<uint8_t[]> track_auto_;
    std::unique_ptr<uint8_t[]> track_auto_last_;
    std::unique_ptr<uint8_t[]> track_ramp_;
    std::unique_ptr<float[]> track_open_last_;
    std::unique_ptr<float[]> auto_level_;
    std::unique_ptr<float[]> auto_pan_;
    // Where the next callback starts if it continues this one, looped, or
    // the playhead, stopped; this callback's start; whether it rolls.
    uint64_t auto_next_ = 0;
    uint64_t auto_start_ = 0;
    bool auto_rolling_ = false;
    bool auto_new_plan_ = true;
    // Live writes (ADR-0027 §3), per engine track, a bit per parameter:
    // what plays its live value instead of its lane this callback and last;
    // what a Latch touch has held since the roll began. Per track and
    // parameter, where a released Touch parameter's return began (kNoReturn:
    // none) and the value, in the lane's unit, it returns from.
    std::unique_ptr<uint8_t[]> auto_live_;
    std::unique_ptr<uint8_t[]> auto_live_last_;
    std::unique_ptr<uint8_t[]> auto_latched_;
    static constexpr int64_t kNoReturn = INT64_MIN;
    std::unique_ptr<int64_t[]> return_at_;
    std::unique_ptr<float[]> return_from_;
    uint32_t touch_return_frames_ = 0;
    std::unique_ptr<RingBuffer<AutomationWrite>> auto_writes_;
    std::atomic<uint64_t> auto_write_overruns_{0};
    std::atomic<uint64_t> auto_passes_ended_{0};
    std::atomic<uint64_t> auto_pass_now_{0};
    std::atomic<uint32_t> auto_unlatch_{0};
    uint32_t auto_unlatch_seen_ = 0;
    uint64_t auto_pass_ = 0;
    bool pass_rolling_ = false;

    // Sends and aux returns (ADR-0022). The atomics are the control
    // thread's to write; the rest the audio thread's.
    struct SendState {
        std::atomic<float> level{0.0f};
        std::atomic<float> pan{0.0f};
        std::atomic<bool> follow_pan{true};
        std::atomic<bool> mute{false};
        std::atomic<bool> pre{false};
    };
    struct AuxState {
        std::atomic<float> gain{1.0f};
        std::atomic<float> pan{0.0f};
        std::atomic<bool> mute{false};
        std::atomic<bool> solo{false};
    };
    // Which nodes and engine tracks feed a soloed aux this callback
    // (ADR-0024 §4), sized when the session is created.
    void feeds_solo(const PlaybackPlan& plan) noexcept SONORA_RT;
    std::unique_ptr<uint8_t[]> feeds_node_;    // per node
    std::unique_ptr<uint8_t[]> feeds_track_;   // per engine track
    uint32_t max_buses_ = 0;
    uint32_t send_states_ = 0;               // (tracks + max_buses) x kStripSendSlots
    std::unique_ptr<SendState[]> sends_;
    std::unique_ptr<AuxState[]> aux_;
    std::unique_ptr<float[]> send_gain_;     // per send, L and R, end of last block
    std::unique_ptr<float[]> send_target_;
    std::unique_ptr<uint64_t[]> send_stamp_; // the callback that last played it
    // The AuxSum's stable id it last fed: Assign Send keeps the slot and
    // changes the bus, and the new bus must hear it ramp in from silence.
    std::unique_ptr<uint32_t[]> send_dest_;
    std::unique_ptr<float[]> aux_gain_;      // per bus, L and R, end of last block
    std::unique_ptr<float[]> aux_target_;
    std::unique_ptr<uint64_t[]> aux_stamp_;
    std::unique_ptr<float[]> aux_open_;      // per bus this block, as track_open_
    std::unique_ptr<float[]> aux_level_;
    std::unique_ptr<float[]> aux_pan_;
    float bus_applied_ = 1.0f;
    float bus_target_ = 1.0f;
    uint64_t seen_epoch_ = 0;
    // A plan that moved tracks between engine tracks has just been adopted:
    // this block's faders start at their targets instead of ramping from
    // what another track left on the engine track (ADR-0023 §4).
    bool snap_mix_ = false;
    uint64_t expected_playhead_ = 0;
    bool reseek_ = true;
    uint32_t block_frames_ = 0;              // the whole block, for ramps
    uint32_t block_done_ = 0;                // frames of it already mixed
    // Frames of this block a roll plays before its roll end; the whole
    // block when it has none in it.
    uint32_t play_until_ = 0;

    std::atomic<float> bus_gain_{1.0f};
    std::atomic<float> bus_peak_[2] = {{0.0f}, {0.0f}};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> locates_dropped_{0};
    std::atomic<uint64_t> chain_bypassed_{0};
    std::atomic<uint64_t> invalid_plan_blocks_{0};
    std::atomic<uint64_t> boundary_overruns_{0};
    std::atomic<uint64_t> params_dropped_{0};
    std::atomic<uint64_t> unrun_chain_blocks_{0};
    std::atomic<uint64_t> silent_chain_blocks_{0};
    // Reset by start_recording() before it publishes the take; set by the
    // audio thread in the take's first block.
    std::atomic<uint64_t> take_origin_{kNoOrigin};
    // The running loop take's wraps, for ClosedTake::loop_wraps. Reset by
    // begin_take() before it publishes the take; written by the audio
    // thread alone.
    std::atomic<uint32_t> loop_wraps_{0};
    // The punched take whose capture_to the audio thread has passed, or 0.
    // Its only writer is the audio thread; begin_take() clears it before
    // the take it starts can be seen.
    std::atomic<uint32_t> punch_passed_{0};
    // set_roll_end(). The audio thread clears it with a compare-exchange
    // once it has stopped the roll, so an end the control thread sets for
    // the next roll meanwhile is kept.
    std::atomic<uint64_t> roll_end_{kNoRollEnd};
    // set_input_latency(). Stored only while no take runs, so the store is
    // ordered before the next take's publication, which the take's first
    // block reads it after.
    std::atomic<uint32_t> input_latency_{0};
    std::unique_ptr<LatencyProbe> probe_;
    std::unique_ptr<Click> click_;
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
