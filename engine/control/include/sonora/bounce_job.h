// SPDX-License-Identifier: GPL-3.0-or-later
//
// One Bounce Mix on behalf of the control server (ui.mixer.bounce): offline
// on a worker thread of its own, or in real time through RealTimeBounce. The
// server starts it from mix.bounce, reads progress each tick and collects the
// outcome on its own thread. Never the audio thread; the real-time mode's
// only audio-thread work is MasterTap::push().
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "sonora/bounce.h"

namespace sonora {

class BounceJob {
public:
    BounceJob() = default;
    BounceJob(const BounceJob&) = delete;
    BounceJob& operator=(const BounceJob&) = delete;
    // Cancels and joins, so a render never outlives the server.
    ~BounceJob();

    struct Result {
        bool ok = false;
        bool cancelled = false;
        BounceResult bounced;
        std::string session_dir;
        std::string error;
    };

    // True from a successful start() until collect(). The control thread's.
    bool active() const { return active_; }
    // Starts `opts` on a copy of `snapshot`. False, `error` set, when it
    // cannot (a real-time bounce needs a stopped transport, say); the job is
    // then not active.
    bool start(Session& session, PlaybackController& playback, const model::Snapshot& snapshot,
               const std::string& session_dir, const api::BounceOptions& opts,
               std::string& error);
    void cancel();
    // The fraction rendered, in thousandths.
    uint32_t permille() const;
    // True once it ended; then collect() joins and hands the outcome over.
    bool finished();
    Result collect();
    // Cancels and drops the outcome (the file is removed): the session this
    // bounce belonged to is gone. True when one was running.
    bool abandon();

private:
    std::unique_ptr<RealTimeBounce> real_time_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint32_t> permille_{0};
    bool active_ = false;
    bool real_time_ended_ = false;
    Result result_;
};

}  // namespace sonora
