// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/stems_job.h"

#include <utility>

namespace sonora {

StemsJob::~StemsJob() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

void StemsJob::start(const model::Snapshot& snapshot, const std::string& session_dir,
                     const api::StemsOptions& opts) {
    if (worker_.joinable()) worker_.join();
    result_ = Result{};
    result_.session_dir = session_dir;
    cancel_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    permille_.store(0, std::memory_order_relaxed);
    said_ = 0;
    active_ = true;
    worker_ = std::thread([this, snap = snapshot, session_dir, opts]() {
        BounceHooks hooks;
        hooks.cancel = &cancel_;
        hooks.progress = [this](double f) {
            permille_.store(static_cast<uint32_t>(f * 1000.0), std::memory_order_relaxed);
        };
        result_.ok = stems_offline(snap, session_dir, opts, hooks, result_.stems, result_.error,
                                   render_);
        finished_.store(true, std::memory_order_release);
    });
}

bool StemsJob::finished() const {
    return active_ && finished_.load(std::memory_order_acquire);
}

StemsJob::Result StemsJob::collect() {
    if (worker_.joinable()) worker_.join();
    active_ = false;
    // Read here, not when the export ended: a cancel that arrived after the
    // last pass still means the operator does not want the stems.
    if (cancel_.load(std::memory_order_relaxed)) result_.cancelled = true;
    return std::move(result_);
}

bool StemsJob::abandon() {
    if (!active_) return false;
    cancel();
    // stems_offline() polls the cancel every block, so the join is short.
    const Result r = collect();
    if (r.ok) remove_stems_folder(r.session_dir, r.stems.folder);
    return true;
}

}  // namespace sonora
