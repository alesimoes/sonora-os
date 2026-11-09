// SPDX-License-Identifier: GPL-3.0-or-later
// Refuses to start recording on a clock that is not there (ADR-0125 §4).
// Clock cannot be fixed after the fact, so the failure is loud and early, at
// session open, never a silent fallback to internal. During a take a lock loss
// is counted, never acted on: stopping transport would lose the take.
// Control thread only.
#pragma once

#include <cstdint>
#include <set>
#include <string>

#include "sonora/clock/clock_source.h"

namespace sonora::clock {

enum class ClockRefusal : std::uint8_t {
    None,
    SourceUnavailable,    // not in caps()
    SelectFailed,         // the card refused the switch
    NotLocked,            // still Unlocked after the bounded polls
    UnknownNotConfirmed,  // card reports no lock and the user has not confirmed
};

struct ClockGuardResult {
    bool ok = false;
    ClockRefusal refusal = ClockRefusal::None;
    // Operator-facing text; shown as a held notice, not the status line.
    std::string reason;
    ClockLock lock = ClockLock::Unknown;
    // Number of lock() reads prepare() spent; never more than max_lock_polls.
    unsigned polls = 0;
};

class ClockGuard {
public:
    // max_lock_polls bounds the wait for lock as a count of reads, never wall
    // clock, so a test drives it deterministically and the UI never hangs.
    ClockGuard(ClockSourceControl& control, std::string device, unsigned max_lock_polls);

    // At session open: resets the per-session state (lock loss count, last
    // lock), selects the source and waits for lock within the poll bound.
    ClockGuardResult prepare(ClockSource requested);

    // The user accepted an external source on a card that reports no lock.
    // Per device (machine settings), not per session.
    void confirm_unknown(const std::string& device);
    bool unknown_confirmed(const std::string& device) const;

    // On the existing control tick: reads lock, counts each Locked -> not
    // Locked transition, keeps the last state. Never touches transport.
    ClockLock poll();

    ClockLock last_lock() const { return last_lock_; }
    std::uint32_t lock_loss_count() const { return lock_loss_count_; }

private:
    ClockSourceControl& control_;
    std::string device_;
    unsigned max_lock_polls_;
    std::set<std::string> confirmed_devices_;
    ClockLock last_lock_ = ClockLock::Unknown;
    std::uint32_t lock_loss_count_ = 0;
};

}  // namespace sonora::clock
