// SPDX-License-Identifier: GPL-3.0-or-later
// AlsaClockControl over FakeClockMixer (ADR-0125 §3). Element names follow
// the kernel's naming rules; they are not observed on a UMC1820.
#include <cstdio>

#include "fake_clock.h"
#include "sonora/clock/clock_mixer.h"

namespace {

using namespace sonora::clock;
using sonora::clock::testing::FakeClockMixer;

int failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failed;
    }
}

void named_card(FakeClockMixer& mixer) {
    mixer.add_enum("USB Clock Source", {"Internal", "ADAT", "S/PDIF", "Word Clock"}, 0);
    mixer.add_bool("Internal Validity", true);
    mixer.add_bool("ADAT Validity", false);
    mixer.add_bool("S/PDIF Validity", true);
    mixer.add_bool("Word Clock Validity", false);
}

void element_name_table_maps_items_to_sources() {
    FakeClockMixer mixer;
    mixer.add_enum("Clock Selector", {"INT", "wclk", "S-PDIF", "ADAT"}, 0);
    AlsaClockControl control(mixer);
    ClockCaps caps = control.caps();
    expect(!caps.offers(ClockSource::Internal), "'INT' is not guessed as internal");
    expect(caps.offers(ClockSource::WordClock), "wclk is word clock");
    expect(caps.offers(ClockSource::Spdif), "S-PDIF spelled with a dash is S/PDIF");
    expect(caps.offers(ClockSource::Adat), "ADAT is ADAT");

    FakeClockMixer other;
    other.add_enum("Foo Bar", {"Internal", "ADAT"});
    AlsaClockControl other_control(other);
    expect(!other_control.caps().offers(ClockSource::Adat),
           "an enum with a non-selector name is ignored");
}

void select_and_selected_round_trip() {
    FakeClockMixer mixer;
    named_card(mixer);
    AlsaClockControl control(mixer);
    expect(control.selected() == ClockSource::Internal, "starts on internal");
    for (ClockSource s : {ClockSource::Spdif, ClockSource::Adat, ClockSource::WordClock,
                          ClockSource::Internal}) {
        ClockResult r = control.select(s);
        expect(r.ok, "select succeeds");
        expect(control.selected() == s, "selected reads back what was selected");
    }
}

void select_failures_carry_a_reason() {
    FakeClockMixer mixer;
    mixer.add_enum("USB Clock Source", {"Internal", "ADAT"}, 0);
    AlsaClockControl control(mixer);
    ClockResult missing = control.select(ClockSource::WordClock);
    expect(!missing.ok && !missing.reason.empty(), "missing item refused with reason");

    mixer.refuse_writes(true);
    ClockResult refused = control.select(ClockSource::Adat);
    expect(!refused.ok && !refused.reason.empty(), "refused write reported");
    expect(control.selected() == ClockSource::Internal, "source unchanged after a refusal");
}

void caps_come_from_the_mixer() {
    FakeClockMixer mixer;
    named_card(mixer);
    AlsaClockControl control(mixer);
    ClockCaps caps = control.caps();
    expect(caps.offers(ClockSource::Internal) && caps.offers(ClockSource::Adat) &&
               caps.offers(ClockSource::Spdif) && caps.offers(ClockSource::WordClock),
           "all four items offered");
    expect(caps.reports_lock, "validity controls mean lock is reported");
}

void lock_follows_the_selected_sources_validity() {
    FakeClockMixer mixer;
    named_card(mixer);
    AlsaClockControl control(mixer);
    control.select(ClockSource::Adat);
    expect(control.lock() == ClockLock::Unlocked, "ADAT invalid reads unlocked");
    mixer.set_bool("ADAT Validity", true);
    expect(control.lock() == ClockLock::Locked, "ADAT valid reads locked");
    control.select(ClockSource::WordClock);
    expect(control.lock() == ClockLock::Unlocked, "lock reads the new source's control");
}

void unnamed_units_pair_with_numbered_validity() {
    FakeClockMixer mixer;
    mixer.add_enum("USB Clock Source", {"Unit 5", "ADAT"}, 0);
    mixer.add_bool("Clock Source 5 Validity", true);
    mixer.add_bool("ADAT Validity", false);
    AlsaClockControl control(mixer);
    expect(control.lock() == ClockLock::Locked, "Unit 5 pairs with Clock Source 5 Validity");
    expect(!control.caps().offers(ClockSource::Internal), "an unnamed unit is not offered");
}

void missing_elements_give_caps_without_lock_reporting() {
    FakeClockMixer empty;
    AlsaClockControl control(empty);
    ClockCaps caps = control.caps();
    expect(!caps.reports_lock, "no validity element: lock not reported");
    expect(caps.offers(ClockSource::Internal), "internal is the one source without a selector");
    expect(!caps.offers(ClockSource::Adat), "nothing else offered");
    expect(control.lock() == ClockLock::Unknown, "lock is Unknown");
    expect(control.select(ClockSource::Internal).ok, "internal needs no switch");
    ClockResult r = control.select(ClockSource::Adat);
    expect(!r.ok && !r.reason.empty(), "external cannot be chosen without a selector");

    FakeClockMixer selector_only;
    selector_only.add_enum("USB Clock Source", {"Internal", "ADAT"}, 1);
    AlsaClockControl bare(selector_only);
    expect(!bare.caps().reports_lock, "selector without validity: lock not reported");
    expect(bare.lock() == ClockLock::Unknown, "and lock reads Unknown");
}

}  // namespace

int main() {
    element_name_table_maps_items_to_sources();
    select_and_selected_round_trip();
    select_failures_carry_a_reason();
    caps_come_from_the_mixer();
    lock_follows_the_selected_sources_validity();
    unnamed_units_pair_with_numbered_validity();
    missing_elements_give_caps_without_lock_reporting();
    return failed == 0 ? 0 : 1;
}
