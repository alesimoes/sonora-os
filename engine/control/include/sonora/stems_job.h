// SPDX-License-Identifier: GPL-3.0-or-later
//
// One stem export on behalf of the control server (ADR-0085): stems_offline()
// on a worker thread of its own, as BounceJob runs an offline bounce. The
// server starts it from mix.stems, reads progress each tick and collects the
// outcome on its own thread. Never the audio thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "sonora/bounce_job.h"
#include "sonora/stems.h"

namespace sonora {

// One export at a time, a bounce or a stem export (ADR-0085 §6): each side
// asks the other through this before it starts.
class ExportGate {
public:
    virtual ~ExportGate() = default;
    virtual bool exporting() const = 0;
};

// The bounce's side of the gate.
class BounceExportGate final : public ExportGate {
public:
    explicit BounceExportGate(const BounceJob& job) : job_(job) {}
    bool exporting() const override { return job_.active(); }

private:
    const BounceJob& job_;
};

class StemsJob final : public ExportGate {
public:
    explicit StemsJob(StemRenderer render = bounce_offline) : render_(std::move(render)) {}
    StemsJob(const StemsJob&) = delete;
    StemsJob& operator=(const StemsJob&) = delete;
    // Cancels and joins, so an export never outlives the server; the folder
    // of one that finished is left to collect() or abandon().
    ~StemsJob() override;

    struct Result {
        bool ok = false;
        bool cancelled = false;
        StemsResult stems;
        std::string session_dir;
        std::string error;
    };

    bool exporting() const override { return active_; }
    // True from start() until collect(). The control thread's.
    bool active() const { return active_; }
    // Starts `opts` on a copy of `snapshot`; stems_check() has passed.
    void start(const model::Snapshot& snapshot, const std::string& session_dir,
               const api::StemsOptions& opts);
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }
    // The fraction of the whole export, in thousandths.
    uint32_t permille() const { return permille_.load(std::memory_order_relaxed); }
    // The permille last announced, so a long export is not a flood of events.
    uint32_t& said() { return said_; }
    // True once it ended; then collect() joins and hands the outcome over.
    bool finished() const;
    Result collect();
    // Cancels, joins and removes whatever was written: the session this
    // export belonged to is gone. True when one was running.
    bool abandon();

private:
    StemRenderer render_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint32_t> permille_{0};
    uint32_t said_ = 0;
    bool active_ = false;
    Result result_;
};

class CommandRegistry;

// The Bounce event, as the server posts it (api/stems.h's meanings).
using StemsAnnounce = std::function<void(api::BounceState state, uint32_t permille,
                                         uint64_t files, const std::string& text)>;

// What mix.stems reads of the server, injected so a test drives it alone.
struct StemsCommandDeps {
    const ExportGate* bounce = nullptr;               // the other export
    std::function<model::Snapshot()> snapshot;        // the document now
    std::function<std::string()> session_dir;         // empty: no folder yet
    std::function<bool()> recording;                  // a take is open
    StemsAnnounce announce;
};

// Installs mix.stems; false when the catalog does not know it.
bool install_stems_commands(CommandRegistry& registry, StemsJob& job, StemsCommandDeps deps);

// The control thread's, each turn: progress while it runs, then how it ended.
// `session_dir_now` is the open session's folder; the stems of one that moved
// or closed meanwhile are removed.
void follow_stems(StemsJob& job, const std::string& session_dir_now,
                  const StemsAnnounce& announce);

}  // namespace sonora
