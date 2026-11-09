// SPDX-License-Identifier: GPL-3.0-or-later
// ClockGuard behaviour (ADR-0125 §4). Lock state is scripted by poll count on
// FakeClockControl; nothing here reads a clock or sleeps.
#include <cstdio>
#include <string>

#include "fake_clock.h"
#include "sonora/clock/clock_guard.h"

namespace {

using namespace sonora::clock;
using sonora::clock::testing::FakeClockControl;

int failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failed;
    }
}

bool mentions(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

ClockCaps adat_caps(bool reports_lock = true) {
    ClockCaps caps;
    caps.offer(ClockSource::Internal);
    caps.offer(ClockSource::Adat);
    caps.reports_lock = reports_lock;
    return caps;
}

void unavailable_source_is_refused_with_a_reason() {
    FakeClockControl control(FakeClockControl::internal_only());
    ClockGuard guard(control, "umc1820", 4);
    ClockGuardResult r = guard.prepare(ClockSource::WordClock);
    expect(!r.ok, "unavailable source refused");
    expect(r.refusal == ClockRefusal::SourceUnavailable, "refusal is SourceUnavailable");
    expect(mentions(r.reason, "word clock") && mentions(r.reason, "umc1820"),
           "reason names source and device");
    expect(control.select_calls() == 0, "card is not asked to switch");
    expect(control.lock_reads() == 0, "lock is not read");
}

void select_failure_is_refused_with_the_cards_reason() {
    FakeClockControl control(adat_caps());
    control.fail_select("busy");
    ClockGuard guard(control, "umc1820", 4);
    ClockGuardResult r = guard.prepare(ClockSource::Adat);
    expect(!r.ok && r.refusal == ClockRefusal::SelectFailed, "select failure refused");
    expect(mentions(r.reason, "busy"), "reason carries the card's text");
    expect(control.selected() == ClockSource::Internal, "no change of source on failure");
}

void unlocked_is_refused_after_bounded_polls() {
    FakeClockControl control(adat_caps());
    control.script_lock({ClockLock::Unlocked});
    ClockGuard guard(control, "umc1820", 3);
    ClockGuardResult r = guard.prepare(ClockSource::Adat);
    expect(!r.ok && r.refusal == ClockRefusal::NotLocked, "unlocked refused");
    expect(r.polls == 3, "exactly the bound of polls");
    expect(control.lock_reads() == 3, "lock read once per poll");
    expect(r.lock == ClockLock::Unlocked, "result carries the lock");
    expect(mentions(r.reason, "ADAT"), "reason names the source");
}

void zero_poll_bound_never_accepts() {
    FakeClockControl control(adat_caps());
    ClockGuard guard(control, "umc1820", 0);
    ClockGuardResult r = guard.prepare(ClockSource::Adat);
    expect(!r.ok && r.refusal == ClockRefusal::NotLocked, "no polls means no acceptance");
    expect(control.lock_reads() == 0, "no reads happened");
}

void lock_arriving_within_the_bound_is_accepted() {
    FakeClockControl control(adat_caps());
    control.script_lock({ClockLock::Unlocked, ClockLock::Unlocked, ClockLock::Locked});
    ClockGuard guard(control, "umc1820", 5);
    ClockGuardResult r = guard.prepare(ClockSource::Adat);
    expect(r.ok && r.refusal == ClockRefusal::None, "accepted once locked");
    expect(r.polls == 3, "stopped polling at lock");
    expect(control.selected() == ClockSource::Adat, "source was selected");
}

void unknown_needs_confirmation_for_an_external_source() {
    FakeClockControl control(adat_caps(false));
    ClockGuard guard(control, "umc1820", 4);
    ClockGuardResult r = guard.prepare(ClockSource::Adat);
    expect(!r.ok && r.refusal == ClockRefusal::UnknownNotConfirmed, "unknown not confirmed");
    expect(r.polls == 1, "unknown ends the wait at once");

    guard.confirm_unknown("umc1820");
    expect(guard.unknown_confirmed("umc1820"), "confirmation is remembered");
    expect(!guard.unknown_confirmed("other"), "confirmation is per device");
    r = guard.prepare(ClockSource::Adat);
    expect(r.ok && r.lock == ClockLock::Unknown, "confirmed unknown is accepted");
}

void internal_needs_no_confirmation_on_a_card_without_validity() {
    FakeClockControl control(adat_caps(false));
    ClockGuard guard(control, "umc1820", 4);
    expect(guard.prepare(ClockSource::Internal).ok, "internal accepted without confirmation");
}

void lock_loss_is_counted_and_transport_untouched() {
    FakeClockControl control(adat_caps());
    control.script_lock({ClockLock::Locked, ClockLock::Unlocked, ClockLock::Unlocked,
                         ClockLock::Locked, ClockLock::Unlocked});
    ClockGuard guard(control, "umc1820", 4);
    expect(guard.prepare(ClockSource::Adat).ok, "prepared");
    const int selects = control.select_calls();

    expect(guard.poll() == ClockLock::Unlocked, "poll reports the loss");
    expect(guard.lock_loss_count() == 1, "first loss counted");
    guard.poll();
    expect(guard.lock_loss_count() == 1, "a continuing loss is not counted again");
    expect(guard.poll() == ClockLock::Locked, "relock seen");
    expect(guard.last_lock() == ClockLock::Locked, "last lock follows");
    guard.poll();
    expect(guard.lock_loss_count() == 2, "second loss after relock counted");
    expect(control.select_calls() == selects, "polling never re-selects or falls back");
    expect(control.selected() == ClockSource::Adat, "source stays external");
}

void prepare_resets_per_session_state() {
    FakeClockControl control(adat_caps());
    control.script_lock({ClockLock::Locked, ClockLock::Unlocked, ClockLock::Locked});
    ClockGuard guard(control, "umc1820", 4);
    guard.prepare(ClockSource::Adat);
    guard.poll();
    expect(guard.lock_loss_count() == 1, "loss in the first session");
    guard.prepare(ClockSource::Adat);
    expect(guard.lock_loss_count() == 0, "new session starts from zero losses");
}

void recovery_after_relock_lets_the_next_prepare_succeed() {
    FakeClockControl control(adat_caps());
    control.script_lock({ClockLock::Unlocked, ClockLock::Unlocked, ClockLock::Locked});
    ClockGuard guard(control, "umc1820", 2);
    expect(!guard.prepare(ClockSource::Adat).ok, "refused while unlocked");
    ClockGuardResult r = guard.prepare(ClockSource::Adat);
    expect(r.ok, "accepted after the card relocks");
    expect(r.refusal == ClockRefusal::None, "no stale refusal");
}

}  // namespace

int main() {
    unavailable_source_is_refused_with_a_reason();
    select_failure_is_refused_with_the_cards_reason();
    unlocked_is_refused_after_bounded_polls();
    zero_poll_bound_never_accepts();
    lock_arriving_within_the_bound_is_accepted();
    unknown_needs_confirmation_for_an_external_source();
    internal_needs_no_confirmation_on_a_card_without_validity();
    lock_loss_is_counted_and_transport_untouched();
    prepare_resets_per_session_state();
    recovery_after_relock_lets_the_next_prepare_succeed();
    return failed == 0 ? 0 : 1;
}
