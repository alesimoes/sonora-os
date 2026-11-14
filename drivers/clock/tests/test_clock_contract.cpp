// SPDX-License-Identifier: GPL-3.0-or-later
// The clock seams' contract, exercised through the fakes the impl and tests
// slices build on (ADR-0125). ClockGuard and AlsaClockControl are tested in
// their own files once they are implemented.
#include <cstdio>
#include <cstring>

#include "fake_clock.h"
#include "sonora/clock/clock_guard.h"
#include "sonora/clock/clock_mixer.h"
#include "sonora/clock/clock_source.h"

namespace {

using namespace sonora::clock;
using sonora::clock::testing::FakeClockControl;
using sonora::clock::testing::FakeClockMixer;

int failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failed;
    }
}

void caps_offer_only_what_was_added() {
    ClockCaps caps;
    expect(!caps.offers(ClockSource::Internal), "empty caps offer nothing");
    caps.offer(ClockSource::Adat);
    expect(caps.offers(ClockSource::Adat), "offered source is offered");
    expect(!caps.offers(ClockSource::WordClock), "other sources stay absent");
    expect(!caps.reports_lock, "lock reporting defaults to off");
    expect(std::strcmp(to_string(ClockSource::Spdif), "S/PDIF") == 0, "source names");
}

void control_refuses_a_source_it_does_not_offer() {
    FakeClockControl control;
    ClockResult r = control.select(ClockSource::WordClock);
    expect(!r.ok, "word clock refused on an internal-only card");
    expect(!r.reason.empty(), "refusal carries a reason");
    expect(control.selected() == ClockSource::Internal, "selection unchanged after refusal");
}

void control_selects_and_reports_a_failed_switch() {
    ClockCaps caps = FakeClockControl::internal_only();
    caps.offer(ClockSource::Adat);
    caps.reports_lock = true;
    FakeClockControl control(caps);
    expect(control.select(ClockSource::Adat).ok, "ADAT selected");
    expect(control.selected() == ClockSource::Adat, "selected() follows select()");

    control.fail_select("card busy");
    ClockResult r = control.select(ClockSource::Internal);
    expect(!r.ok && r.reason == "card busy", "scripted select failure");
    expect(control.selected() == ClockSource::Adat, "failed switch keeps the old source");
    expect(control.select_calls() == 2, "select calls counted");
}

void control_lock_follows_the_script_then_sticks() {
    ClockCaps caps = FakeClockControl::internal_only();
    caps.reports_lock = true;
    FakeClockControl control(caps);
    expect(control.lock() == ClockLock::Locked, "unscripted lock-reporting card is Locked");
    control.script_lock({ClockLock::Unlocked, ClockLock::Unlocked, ClockLock::Locked});
    expect(control.lock() == ClockLock::Unlocked, "script step 1");
    expect(control.lock() == ClockLock::Unlocked, "script step 2");
    expect(control.lock() == ClockLock::Locked, "script step 3");
    expect(control.lock() == ClockLock::Locked, "last state sticks");
    expect(control.lock_reads() == 5, "lock reads counted");

    FakeClockControl silent;
    expect(silent.lock() == ClockLock::Unknown, "card without validity reports Unknown");
}

void mixer_enumerates_and_reads_back_writes() {
    FakeClockMixer mixer;
    mixer.add_enum("Clock Selector", {"Internal", "ADAT", "S/PDIF"}, 0);
    mixer.add_bool("Clock Source 1 Validity", true);
    expect(mixer.elements().size() == 2, "two elements enumerated");

    expect(mixer.write_enum("Clock Selector", 1), "enum write");
    expect(mixer.read_enum("Clock Selector") == std::optional<std::size_t>(1), "enum read back");
    expect(!mixer.write_enum("Clock Selector", 3), "out-of-range index refused");

    expect(mixer.read_bool("Clock Source 1 Validity") == std::optional<bool>(true), "bool read");
    mixer.set_bool("Clock Source 1 Validity", false);
    expect(mixer.read_bool("Clock Source 1 Validity") == std::optional<bool>(false), "cable pulled");
    expect(mixer.writes() == 1, "only accepted writes counted");
}

void mixer_reports_absent_and_mistyped_elements() {
    FakeClockMixer mixer;
    mixer.add_bool("Clock Source 1 Validity", true);
    expect(!mixer.read_enum("Clock Selector").has_value(), "absent enum reads nullopt");
    expect(!mixer.read_enum("Clock Source 1 Validity").has_value(), "bool is not an enum");
    expect(!mixer.write_bool("Nope", true), "absent bool write refused");

    mixer.refuse_writes(true);
    expect(!mixer.write_bool("Clock Source 1 Validity", false), "card refusing writes");
    expect(mixer.read_bool("Clock Source 1 Validity") == std::optional<bool>(true),
           "refused write leaves the value");
}

void guard_result_defaults_to_refused() {
    ClockGuardResult r;
    expect(!r.ok, "a default result is not permission to record");
    expect(r.refusal == ClockRefusal::None && r.polls == 0, "result defaults");
}

}  // namespace

int main() {
    caps_offer_only_what_was_added();
    control_refuses_a_source_it_does_not_offer();
    control_selects_and_reports_a_failed_switch();
    control_lock_follows_the_script_then_sticks();
    mixer_enumerates_and_reads_back_writes();
    mixer_reports_absent_and_mistyped_elements();
    guard_result_defaults_to_refused();
    expect(std::strcmp(clock_module_name(), "sonora_clock") == 0, "library links");
    if (failed != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failed);
        return 1;
    }
    std::printf("clock contract: all checks passed\n");
    return 0;
}
