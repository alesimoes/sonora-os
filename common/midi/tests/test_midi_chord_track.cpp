// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/chord_track.h"

#include "check.h"

using namespace sonora::midi;

namespace {

Chord ch(const char* s) { return *parse_chord(s); }

int sorted_one_per_tick() {
    ChordTrack t;
    t.set(960, ch("G"));
    t.set(0, ch("C"));
    t.set(480, ch("Am"));
    CHECK(t.size() == 3);
    CHECK(t.changes()[0].tick == 0 && t.changes()[2].tick == 960);
    return 0;
}

int replace_at_tick() {
    ChordTrack t;
    t.set(480, ch("Am"));
    t.set(480, ch("F"));
    CHECK(t.size() == 1);
    CHECK(*t.chord_at(480) == ch("F"));
    return 0;
}

int lookup() {
    ChordTrack t;
    CHECK(!t.chord_at(0));
    t.set(480, ch("C"));
    t.set(960, ch("G"));
    CHECK(!t.chord_at(479));
    CHECK(*t.chord_at(480) == ch("C"));
    CHECK(*t.chord_at(959) == ch("C"));
    CHECK(*t.chord_at(960) == ch("G"));
    CHECK(*t.chord_at(100000) == ch("G"));
    return 0;
}

int removal() {
    ChordTrack t;
    t.set(0, ch("C"));
    CHECK(!t.remove(5));
    CHECK(t.remove(0));
    CHECK(t.empty());
    return 0;
}

}  // namespace

int main() {
    RUN(sorted_one_per_tick);
    RUN(replace_at_tick);
    RUN(lookup);
    RUN(removal);
    return 0;
}
