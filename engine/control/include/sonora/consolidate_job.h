// SPDX-License-Identifier: GPL-3.0-or-later
//
// One consolidate render on a worker thread (ADR-0031 §6): the control
// server starts it from session.consolidate, reads its progress each tick,
// and commits the file to the document on its own thread once it is done.
// Never the audio thread, which it does not touch.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "sonora/consolidate.h"

namespace sonora {

class ConsolidateJob {
public:
    ConsolidateJob() = default;
    ConsolidateJob(const ConsolidateJob&) = delete;
    ConsolidateJob& operator=(const ConsolidateJob&) = delete;
    // Cancels and joins, so a render never outlives the server.
    ~ConsolidateJob();

    struct Result {
        bool ok = false;
        bool cancelled = false;
        ConsolidateRequest request;   // as resolved by consolidate_check()
        RenderedRange rendered;
        std::string session_dir;
        std::string error;
    };

    // True from start() until collect(). The control thread's.
    bool active() const { return active_; }
    // Renders `request` of `snapshot` into `session_dir`. Only when not active().
    void start(model::Snapshot snapshot, std::string session_dir, ConsolidateRequest request);
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }
    // The fraction rendered, in thousandths.
    uint32_t permille() const { return permille_.load(std::memory_order_relaxed); }
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    // Joins the worker and hands over what it made. Only when finished().
    Result collect();

private:
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint32_t> permille_{0};
    bool active_ = false;
    Result result_;
};

}  // namespace sonora
