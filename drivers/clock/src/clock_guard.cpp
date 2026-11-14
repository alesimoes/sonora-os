// SPDX-License-Identifier: GPL-3.0-or-later
// ClockGuard (ADR-0125 §4). Every refusal names the source and the device so
// the operator can act on it; nothing here falls back to Internal on its own,
// because a take on the wrong clock drifts against the other devices with no
// sign in the audio.
#include "sonora/clock/clock_guard.h"

#include <utility>

namespace sonora::clock {

namespace {

std::string source_on(ClockSource source, const std::string& device) {
    return std::string(to_string(source)) + " clock on " + device;
}

ClockGuardResult refuse(ClockRefusal why, std::string reason, ClockLock lock, unsigned polls) {
    ClockGuardResult r;
    r.ok = false;
    r.refusal = why;
    r.reason = std::move(reason) + "; recording stays disabled.";
    r.lock = lock;
    r.polls = polls;
    return r;
}

ClockGuardResult accept(ClockLock lock, unsigned polls) {
    ClockGuardResult r;
    r.ok = true;
    r.lock = lock;
    r.polls = polls;
    return r;
}

struct LockWait {
    ClockLock lock = ClockLock::Unknown;
    unsigned polls = 0;
};

// Reads lock until Locked, or Unknown (the card reports no validity, so more
// reads cannot change the answer), or the poll bound runs out.
LockWait wait_for_lock(const ClockSourceControl& control, unsigned max_polls) {
    LockWait w;
    while (w.polls < max_polls) {
        w.lock = control.lock();
        ++w.polls;
        if (w.lock != ClockLock::Unlocked) break;
    }
    return w;
}

}  // namespace

ClockGuard::ClockGuard(ClockSourceControl& control, std::string device, unsigned max_lock_polls)
    : control_(control), device_(std::move(device)), max_lock_polls_(max_lock_polls) {}

ClockGuardResult ClockGuard::prepare(ClockSource requested) {
    // A new session starts from no knowledge: a loss counted in the previous
    // session must not show as a warning in this one.
    last_lock_ = ClockLock::Unknown;
    lock_loss_count_ = 0;

    const std::string what = source_on(requested, device_);
    if (!control_.caps().offers(requested))
        return refuse(ClockRefusal::SourceUnavailable, what + " is not offered by the card",
                      ClockLock::Unknown, 0);

    ClockResult selected = control_.select(requested);
    if (!selected.ok)
        return refuse(ClockRefusal::SelectFailed,
                      "could not switch to " + what + ": " + selected.reason, ClockLock::Unknown, 0);

    if (max_lock_polls_ == 0)
        return refuse(ClockRefusal::NotLocked, what + ": lock was not checked (poll bound is 0)",
                      ClockLock::Unknown, 0);

    LockWait w = wait_for_lock(control_, max_lock_polls_);
    last_lock_ = w.lock;
    if (w.lock == ClockLock::Unlocked)
        return refuse(ClockRefusal::NotLocked,
                      what + " is not locked after " + std::to_string(w.polls) +
                          " checks; check the cable and the sending device's rate",
                      w.lock, w.polls);

    // The internal clock is the card's own, so a card that cannot report
    // validity needs no confirmation for it; an external one does.
    if (w.lock == ClockLock::Unknown && requested != ClockSource::Internal &&
        !unknown_confirmed(device_))
        return refuse(ClockRefusal::UnknownNotConfirmed,
                      what + ": the card does not report lock; confirm the source is connected",
                      w.lock, w.polls);

    return accept(w.lock, w.polls);
}

void ClockGuard::confirm_unknown(const std::string& device) { confirmed_devices_.insert(device); }

bool ClockGuard::unknown_confirmed(const std::string& device) const {
    return confirmed_devices_.count(device) != 0;
}

ClockLock ClockGuard::poll() {
    // Only counted and kept: the UI warns, transport keeps running, because
    // stopping on a lost clock would lose the take (ADR-0125 §4).
    ClockLock now = control_.lock();
    if (last_lock_ == ClockLock::Locked && now != ClockLock::Locked) ++lock_loss_count_;
    last_lock_ = now;
    return now;
}

}  // namespace sonora::clock
