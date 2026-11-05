// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounce Mix, the engine's half (ui.mixer.bounce): the mix as playback plays
// it, faders, pan, mutes, sends, buses and automation, written to one
// stereo file in "Bounced Files/".
//
// Offline: a private Session plays the document on the calling thread, which
// is a worker's, as fast as the disk reader serves it. Plug-in inserts are
// not hosted there, so a session with an active insert is refused and the
// real-time mode, which records the live master, is the way to bounce it.
//
// Real time: the live session plays and its audio thread pushes the master
// into a MasterTap (a RingBuffer); a worker drains it to disk. A full ring
// is counted as lost frames and the bounce is then refused at the end.
//
// Neither touches the document. Neither runs on the audio thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "sonora/api/bounce.h"
#include "sonora/model/model.h"

namespace sonora {

class Session;
class PlaybackController;
class MasterTap;

struct BounceHooks {
    // The fraction rendered, in (0, 1], from the rendering thread.
    std::function<void(double)> progress;
    // Polled each block; set from another thread to stop. The file is then
    // removed.
    const std::atomic<bool>* cancel = nullptr;
};

struct BounceResult {
    std::string relative_path;   // "Bounced Files/<name>.wav"
    uint64_t frames = 0;
    double integrated_lufs = -200.0;   // of the file as written
    double peak_dbfs = -200.0;         // sample peak of the file as written
    double gain_db = 0.0;              // the normalise gain applied, 0 when off
};

inline constexpr const char* kBouncedDir = "Bounced Files";

// The frame after the last region on any audio track's active playlist.
model::Frames bounce_extent(const model::Snapshot& snapshot);

// What a bounce start refuses at once, with its reason: an empty range, and
// for the offline mode a session with an active plug-in insert.
bool bounce_check(const model::Snapshot& snapshot, const api::BounceOptions& opts,
                  std::string& error);

// Renders offline. `opts.mode` is ignored. On failure `error` says why and
// no file is left behind.
bool bounce_offline(const model::Snapshot& snapshot, const std::string& session_dir,
                    const api::BounceOptions& opts, const BounceHooks& hooks, BounceResult& out,
                    std::string& error);

// Real-time bounce of the live session. Control thread: start(), poll()
// every turn, cancel(). The transport must be stopped when it starts.
class RealTimeBounce {
public:
    RealTimeBounce(Session& session, PlaybackController& playback);
    ~RealTimeBounce();

    RealTimeBounce(const RealTimeBounce&) = delete;
    RealTimeBounce& operator=(const RealTimeBounce&) = delete;

    // Locates to the start (waiting for the window, as locate/settle do),
    // opens the file, taps the master and rolls to the end. Refused while a
    // take is open or the transport is not stopped.
    bool start(const model::Snapshot& snapshot, const std::string& session_dir,
               const api::BounceOptions& opts, std::string& error);
    bool running() const { return worker_.joinable(); }
    double progress() const { return progress_.load(std::memory_order_relaxed); }
    // Stops the roll and the worker; the file is removed.
    void cancel();
    // True once it ended: `out` is filled, or `error` says why not
    // (`cancelled` when cancel() did). Joins the worker.
    bool poll(BounceResult& out, std::string& error, bool& cancelled);

private:
    struct Run;
    void work();
    void release_tap();

    Session& session_;
    PlaybackController& playback_;
    std::unique_ptr<MasterTap> tap_;
    std::unique_ptr<Run> run_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<double> progress_{0.0};
};

}  // namespace sonora
