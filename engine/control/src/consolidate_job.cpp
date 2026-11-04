// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/consolidate_job.h"

#include <utility>

namespace sonora {

ConsolidateJob::~ConsolidateJob() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

void ConsolidateJob::start(model::Snapshot snapshot, std::string session_dir,
                           ConsolidateRequest request) {
    cancel_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    permille_.store(0, std::memory_order_relaxed);
    result_ = Result{};
    result_.request = std::move(request);
    result_.session_dir = std::move(session_dir);
    active_ = true;
    worker_ = std::thread([this, snap = std::move(snapshot)]() {
        ConsolidateHooks hooks;
        hooks.cancel = &cancel_;
        hooks.progress = [this](double f) {
            permille_.store(static_cast<uint32_t>(f * 1000.0), std::memory_order_relaxed);
        };
        result_.ok = render_range(snap, result_.session_dir, result_.request, hooks,
                                  result_.rendered, result_.error);
        finished_.store(true, std::memory_order_release);
    });
}

ConsolidateJob::Result ConsolidateJob::collect() {
    if (worker_.joinable()) worker_.join();
    active_ = false;
    // Read here, not when the render ended: a cancel that arrived after the
    // last block still means the operator does not want the result.
    result_.cancelled = cancel_.load(std::memory_order_relaxed);
    return std::move(result_);
}

}  // namespace sonora
