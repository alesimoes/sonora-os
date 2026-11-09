// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place, the engine's half (ADR-0051; plugins.graph.freeze,
// plugins.offline): print one track through its own plug-in chain to a file
// in "Audio Files/" and put the file in the track as Freeze, Commit or
// Process Selection (model/render_in_place.h).
//
// In real time, by design. The chain's hosts are other people's code in
// processes of their own (ADR-0004), and they run only at the audio
// callback (ADR-0012): there is no host to drive from a worker. So the live
// session plays the range with the transport and its audio thread pushes
// what the track's chain played into a TrackTap (a RingBuffer); a worker
// drains it to disk. The chain's output is its latency late, so the worker
// drops that many frames from the head and the file lands on the timeline
// sample-accurately. A block the chain did not play, a block the disk did
// not serve, a frame the ring had no room for, or a chain whose latency
// moved fails the render and no file is kept (ADR-0012 §3).
//
// The worker never touches the document; the control thread applies the
// compound when poll() hands the file over (place_render()).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/render_in_place.h"
#include "sonora/model/replication.h"

namespace sonora {

namespace api {
struct RenderOptions;
}

class Session;
class PlaybackController;
class RenderSolo;
class TrackTap;

inline constexpr uint32_t kRenderPrerollMs = 1000;   // warms dynamics before the range
inline constexpr uint32_t kRenderFreezeTailMs = 2000;
// How long the hosts get to confirm a render-scoped solo (ADR-0087 §4)
// before the render fails: a host reads its bypass at its next block.
inline constexpr uint32_t kRenderSoloConfirmMs = 2000;

struct RenderRequest {
    model::RenderKind kind = model::RenderKind::Freeze;
    model::Id track = 0;
    // The range as the model asked for it (render_range_of()), before the tail.
    model::Frames start = 0;
    model::Frames end = 0;
    model::Frames tail = 0;
    // Process only (ADR-0087): the one insert the selection goes through,
    // the others silenced for the render; 0 is the whole active chain.
    model::Id only_insert = 0;
    std::string name;   // empty: "<track> Frozen", "<track> Committed", "<track> Processed"
};

struct RenderResult {
    std::string relative_path;   // "Audio Files/<name>.wav"
    uint64_t frames = 0;         // in the file
    // Where the file's first frame is on the timeline.
    model::Frames start = 0;
    std::string label;           // what the notice says: "Vox frozen"
};

// What the state of the track was when the render began, so a render of a
// track edited meanwhile is refused instead of put over the edit.
struct RenderBasis {
    std::shared_ptr<const model::TrackEdit> track;
    // The bypass of each active insert then (insert id, bypassed): a bypass
    // is mix state, not an edit, and it changes what the chain plays.
    std::vector<std::pair<model::Id, bool>> bypass;
};

// The bypass of `track`'s active inserts in `snapshot`.
std::vector<std::pair<model::Id, bool>> insert_bypass(const model::Snapshot& snapshot,
                                                      const model::TrackEdit& track);

class RenderInPlace {
public:
    // `chain_misses`: the blocks the track's chain has not played from its
    // host, read now (PluginHosting::chain_misses); two readings that differ
    // fail the render.
    using ChainMisses = std::function<uint64_t()>;

    RenderInPlace(Session& session, PlaybackController& playback);
    ~RenderInPlace();
    RenderInPlace(const RenderInPlace&) = delete;
    RenderInPlace& operator=(const RenderInPlace&) = delete;

    // The override a request with `only_insert` needs (ADR-0087 §4); it
    // outlives this object. Without one such a request is refused, never
    // printed through the whole chain.
    void set_solo(RenderSolo* solo) { solo_ = solo; }

    // Locates before the range (waiting for the window, as locate/settle do),
    // opens the file, taps the track and rolls over it; with `only_insert`,
    // silences the other inserts first and waits for the hosts to confirm
    // it. Refused while a take is open or the transport is not stopped, and
    // for a track the session has no engine strip for.
    bool start(const model::Snapshot& snapshot, const std::string& session_dir,
               const RenderRequest& request, ChainMisses chain_misses, std::string& error);
    bool running() const { return worker_.joinable(); }
    double progress() const { return progress_.load(std::memory_order_relaxed); }
    const RenderRequest& request() const { return request_; }
    const RenderBasis& basis() const { return basis_; }
    // Stops the roll and the worker; the file is removed.
    void cancel();
    // True once it ended: `out` is filled, or `error` says why not
    // (`cancelled` when cancel() did). Joins the worker and releases the tap.
    bool poll(RenderResult& out, std::string& error, bool& cancelled);

private:
    struct Run;
    void work();
    void release_tap();
    // render_solo.cpp: the override posted and confirmed (false with
    // `error` when a host did not confirm it), and lifted on every exit.
    bool engage_solo(model::Id track, model::Id insert, std::string& error);
    void lift_solo();

    Session& session_;
    PlaybackController& playback_;
    std::unique_ptr<TrackTap> tap_;
    std::unique_ptr<Run> run_;
    RenderRequest request_;
    RenderBasis basis_;
    ChainMisses chain_misses_;
    RenderSolo* solo_ = nullptr;
    model::Id soloed_track_ = 0;   // 0: no override standing
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<double> progress_{0.0};
};

// Control thread. Puts the rendered file in the document as one compound
// (AddSource, then model::render_members()) labelled "Freeze Track", "Commit
// Track" or "Process Selection", and gives `updates` the one update to send
// every replica. The file is removed on any failure, and `error` says why;
// the document is then unchanged. Refused when the track changed since
// `basis`.
bool place_render(model::Document& document, const std::string& session_dir,
                  const RenderRequest& request, const RenderBasis& basis,
                  const RenderResult& rendered, std::vector<model::ReplicationUpdate>& updates,
                  std::string& error);

// Control thread. Unfreeze as one compound labelled "Unfreeze Track".
bool unfreeze_track(model::Document& document, model::Id track,
                    std::vector<model::ReplicationUpdate>& updates, std::string& error);

struct RenderAsk {
    model::Id track = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    int64_t tail_ms = -1;   // -1: the kind's own
    model::Id only_insert = 0;
    std::string name;
    uint32_t sample_rate = 48000;

    // What the wire asked for, at the session's rate.
    static RenderAsk from_options(const api::RenderOptions& o, uint32_t sample_rate);
};

// The request a command asks for, checked against the document: the range of
// the track's audio (Freeze, Commit) or the selection (Process), the tail,
// and the refusals of the model in the operator's words. `only_insert` is
// Process Selection's only and must be an active, unbypassed insert of the
// track (model::render_insert_valid).
bool plan_render(const model::Snapshot& snapshot, model::RenderKind kind, const RenderAsk& ask,
                 RenderRequest& out, std::string& error);

}  // namespace sonora
