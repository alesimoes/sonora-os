// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/bounce_job.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

namespace sonora {

BounceJob::~BounceJob() {
    cancel();
    if (worker_.joinable()) worker_.join();
    // RealTimeBounce's destructor stops its own worker and the tap.
}

bool BounceJob::start(Session& session, PlaybackController& playback,
                      const model::Snapshot& snapshot, const std::string& session_dir,
                      const api::BounceOptions& opts, std::string& error) {
    result_ = Result{};
    result_.session_dir = session_dir;
    cancel_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    permille_.store(0, std::memory_order_relaxed);
    real_time_ended_ = false;
    if (opts.mode == api::BounceMode::RealTime) {
        real_time_ = std::make_unique<RealTimeBounce>(session, playback);
        if (!real_time_->start(snapshot, session_dir, opts, error)) {
            real_time_.reset();
            return false;
        }
        active_ = true;
        return true;
    }
    active_ = true;
    worker_ = std::thread([this, snap = snapshot, session_dir, opts]() {
        BounceHooks hooks;
        hooks.cancel = &cancel_;
        hooks.progress = [this](double f) {
            permille_.store(static_cast<uint32_t>(f * 1000.0), std::memory_order_relaxed);
        };
        result_.ok = bounce_offline(snap, session_dir, opts, hooks, result_.bounced, result_.error);
        finished_.store(true, std::memory_order_release);
    });
    return true;
}

void BounceJob::cancel() {
    cancel_.store(true, std::memory_order_relaxed);
    if (real_time_) real_time_->cancel();
}

uint32_t BounceJob::permille() const {
    if (real_time_) {
        return static_cast<uint32_t>(real_time_->progress() * 1000.0);
    }
    return permille_.load(std::memory_order_relaxed);
}

bool BounceJob::finished() {
    if (!active_) return false;
    if (real_time_) {
        if (!real_time_ended_) {
            real_time_ended_ = real_time_->poll(result_.bounced, result_.error, result_.cancelled);
            result_.ok = real_time_ended_ && result_.error.empty() && !result_.cancelled;
        }
        return real_time_ended_;
    }
    return finished_.load(std::memory_order_acquire);
}

BounceJob::Result BounceJob::collect() {
    if (worker_.joinable()) worker_.join();
    active_ = false;
    real_time_.reset();
    // Read here, not when the render ended: a cancel that arrived after the
    // last block still means the operator does not want the result.
    if (cancel_.load(std::memory_order_relaxed)) result_.cancelled = true;
    return std::move(result_);
}

bool BounceJob::abandon() {
    if (!active_) return false;
    cancel();
    while (!finished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Result r = collect();
    if (r.ok) std::remove((r.session_dir + "/" + r.bounced.relative_path).c_str());
    return true;
}

}  // namespace sonora
