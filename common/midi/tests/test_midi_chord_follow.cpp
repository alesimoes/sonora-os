// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/chord_follow.h"

#include "check.h"

using namespace sonora::midi;

namespace {

Chord ch(const char* s) { return *parse_chord(s); }

Note note(Tick start, int pitch) {
    Note n;
    n.start = start;
    n.length = 100;
    n.pitch = static_cast<std::uint8_t>(pitch);
    return n;
}

int transpose_up_and_down() {
    ChordTrack t;
    t.set(0, ch("C"));
    t.set(480, ch("G"));  // +7 is -5 the short way
    t.set(960, ch("D"));  // +2
    const NoteList in{note(0, 60), note(480, 60), note(960, 60)};
    const NoteList out = follow_chords(in, t, ch("C"), FollowMode::Transpose);
    CHECK(out[0].pitch == 60 && out[1].pitch == 55 && out[2].pitch == 62);
    return 0;
}

int tritone_goes_down() {
    ChordTrack t;
    t.set(0, ch("F#"));
    const NoteList out = follow_chords({note(0, 60)}, t, ch("C"), FollowMode::Transpose);
    CHECK(out[0].pitch == 54);  // -6, not +6
    return 0;
}

int before_first_chord_unchanged() {
    ChordTrack t;
    t.set(480, ch("D"));
    const NoteList out =
        follow_chords({note(0, 60), note(480, 60)}, t, ch("C"), FollowMode::Transpose);
    CHECK(out[0].pitch == 60 && out[1].pitch == 62);
    return 0;
}

int empty_track_unchanged() {
    const ChordTrack t;
    const NoteList in{note(0, 61)};
    CHECK(follow_chords(in, t, ch("C"), FollowMode::Transpose) == in);
    CHECK(follow_chords(in, t, ch("C"), FollowMode::SnapToChord) == in);
    return 0;
}

int snap() {
    ChordTrack t;
    t.set(0, ch("C"));  // C E G
    const NoteList in{note(0, 60), note(0, 61), note(0, 62), note(0, 63), note(0, 66)};
    const NoteList out = follow_chords(in, t, ch("A"), FollowMode::SnapToChord);
    CHECK(out[0].pitch == 60);
    CHECK(out[1].pitch == 60);  // C# is nearer C than E
    CHECK(out[2].pitch == 60);  // D is two from C and from E: the lower wins
    CHECK(out[3].pitch == 64);  // D# is nearer E
    CHECK(out[4].pitch == 67);  // F# is nearer G
    return 0;
}

int range_edges() {
    ChordTrack t;
    t.set(0, ch("B"));  // -1 from C
    NoteList out = follow_chords({note(0, 0)}, t, ch("C"), FollowMode::Transpose);
    CHECK(out[0].pitch == 11);  // -1 comes back up an octave
    ChordTrack u;
    u.set(0, ch("D"));  // +2
    out = follow_chords({note(0, 127)}, u, ch("C"), FollowMode::Transpose);
    CHECK(out[0].pitch == 117);  // 129 comes back down an octave
    return 0;
}

int source_untouched() {
    ChordTrack t;
    t.set(0, ch("D"));
    const NoteList in{note(0, 60)};
    follow_chords(in, t, ch("C"), FollowMode::Transpose);
    CHECK(in[0].pitch == 60);
    return 0;
}

}  // namespace

int main() {
    RUN(transpose_up_and_down);
    RUN(tritone_goes_down);
    RUN(before_first_chord_unchanged);
    RUN(empty_track_unchanged);
    RUN(snap);
    RUN(range_edges);
    RUN(source_untouched);
    return 0;
}
