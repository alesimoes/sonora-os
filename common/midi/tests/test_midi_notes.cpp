// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/notes.h"

#include "check.h"

using namespace sonora::midi;

namespace {

Note make(Tick start, Tick length, std::uint8_t pitch, std::uint8_t velocity = 100,
          std::uint8_t channel = 0, std::uint8_t release = default_release_velocity) {
    Note n;
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = velocity;
    n.channel = channel;
    n.release_velocity = release;
    return n;
}

NoteList round_trip(const NoteList& notes) {
    return extract_notes(notes_to_events(notes)).notes;
}

int pairs_on_and_off() {
    const EventList events{
        program_change(0, 0, 5),
        note_on(0, 0, 60, 90),
        note_on(0, 0, 64, 80),
        control_change(100, 0, 64, 127),
        note_off(480, 0, 60, 33),
        note_on(960, 0, 64, 0),  // velocity 0 ends the note
        end_of_track(1000),
    };
    const NoteExtraction x = extract_notes(events);
    CHECK(x.notes.size() == 2);
    CHECK(x.notes[0] == make(0, 480, 60, 90, 0, 33));
    // A note-on of velocity 0 carries no release velocity: the default.
    CHECK(x.notes[1] == make(0, 960, 64, 80, 0, default_release_velocity));
    CHECK(x.other.size() == 3);
    CHECK(x.other[0].kind() == Kind::ProgramChange);
    CHECK(x.other[2].is_end_of_track());
    CHECK(x.unmatched_note_offs == 0 && x.unterminated_notes == 0);
    return 0;
}

int channels_are_separate() {
    const EventList events{note_on(0, 0, 60, 1), note_on(10, 1, 60, 2),
                           note_off(20, 1, 60), note_off(30, 0, 60)};
    const NoteExtraction x = extract_notes(events);
    CHECK(x.notes.size() == 2);
    CHECK(x.notes[0].channel == 0 && x.notes[0].length == 30);
    CHECK(x.notes[1].channel == 1 && x.notes[1].length == 10);
    return 0;
}

int overlapping_same_pitch_is_first_in_first_out() {
    // Staggered: B starts while A sounds and ends after it.
    const EventList events{note_on(0, 0, 60, 10), note_on(50, 0, 60, 20),
                           note_off(100, 0, 60), note_off(150, 0, 60)};
    const NoteExtraction x = extract_notes(events);
    CHECK(x.notes.size() == 2);
    CHECK(x.notes[0] == make(0, 100, 60, 10));
    CHECK(x.notes[1] == make(50, 100, 60, 20));

    // And that shape survives notes -> events -> notes.
    CHECK(round_trip(x.notes) == x.notes);
    return 0;
}

int nested_same_pitch_becomes_staggered() {
    // B lies inside A. No byte stream can say which off is whose; FIFO
    // returns two notes over the same span, documented in notes.h.
    const NoteList nested{make(0, 200, 60, 10), make(50, 50, 60, 20)};
    const NoteList back = round_trip(nested);
    CHECK(back.size() == 2);
    CHECK(back[0] == make(0, 100, 60, 10));
    CHECK(back[1] == make(50, 150, 60, 20));
    return 0;
}

int same_start_same_pitch_survives() {
    // Equal starts: the shorter is ended first, so lengths come back.
    const NoteList notes{make(0, 100, 60, 10), make(0, 50, 60, 20)};
    const NoteList back = round_trip(notes);
    NoteList expected = notes;
    sort_notes(expected);
    CHECK(expected[0].length == 50);
    CHECK(back == expected);
    return 0;
}

int back_to_back_notes_are_not_cut() {
    // One ends exactly where the next of the same pitch starts: the off
    // must be written before the on at that tick.
    const NoteList notes{make(0, 100, 60), make(100, 100, 60)};
    const EventList events = notes_to_events(notes);
    CHECK(events.size() == 4);
    CHECK(events[1].tick == 100 && events[1].is_note_off());
    CHECK(events[2].tick == 100 && events[2].is_note_on());
    CHECK(round_trip(notes) == notes);
    return 0;
}

int zero_length_and_velocity_edges() {
    const NoteList notes{make(10, 0, 60), make(10, 20, 60), make(30, 0, 61)};
    NoteList expected = notes;
    sort_notes(expected);
    CHECK(round_trip(notes) == expected);

    // Velocity 0 would be read as a note-off; it is written as 1.
    const EventList events = notes_to_events({make(0, 10, 60, 0)});
    CHECK(events[0].is_note_on() && events[0].velocity() == 1);
    return 0;
}

int unmatched_and_unterminated() {
    const EventList events{note_off(0, 0, 60), note_on(10, 0, 62, 1),
                           note_on(20, 0, 62, 1), note_off(30, 0, 62),
                           end_of_track(500)};
    const NoteExtraction x = extract_notes(events);
    CHECK(x.unmatched_note_offs == 1);
    CHECK(x.unterminated_notes == 1);
    CHECK(x.notes.size() == 2);
    CHECK(x.notes[0] == make(10, 20, 62, 1));
    // Still sounding at the end: ended at the end of track.
    CHECK(x.notes[1] == make(20, 480, 62, 1));
    return 0;
}

int unsorted_input_is_read_in_tick_order() {
    const EventList events{note_off(100, 0, 60), note_on(0, 0, 60, 1)};
    const NoteExtraction x = extract_notes(events);
    CHECK(x.notes.size() == 1 && x.notes[0].length == 100);
    CHECK(x.unmatched_note_offs == 0);
    return 0;
}

int many_notes_round_trip() {
    NoteList notes;
    for (int i = 0; i < 500; ++i) {
        notes.push_back(make(i * 37 % 4000, 1 + i * 13 % 300,
                             static_cast<std::uint8_t>(30 + i % 50),
                             static_cast<std::uint8_t>(1 + i % 127),
                             static_cast<std::uint8_t>(i % 3),
                             static_cast<std::uint8_t>(i % 128)));
    }
    // Remove same-pitch nesting, the one shape that cannot round-trip.
    sort_notes(notes);
    NoteList clean;
    for (const Note& n : notes) {
        bool nested = false;
        for (const Note& m : clean) {
            if (m.channel == n.channel && m.pitch == n.pitch && m.end() > n.start) {
                nested = true;
            }
        }
        if (!nested) clean.push_back(n);
    }
    CHECK(clean.size() > 100);
    CHECK(round_trip(clean) == clean);
    return 0;
}

int merge_events_orders_and_keeps_one_end() {
    const EventList a{program_change(0, 0, 1), note_on(10, 0, 60, 1), end_of_track(50)};
    const EventList b{control_change(0, 0, 7, 100), note_on(5, 0, 61, 1),
                      note_off(80, 0, 61), end_of_track(70)};
    const EventList m = merge_events(a, b);
    CHECK(m.size() == 6);
    CHECK(m[0].kind() == Kind::ProgramChange);  // a before b at one tick
    CHECK(m[1].kind() == Kind::ControlChange);
    CHECK(m[2].tick == 5 && m[3].tick == 10 && m[4].tick == 80);
    CHECK(m[5].is_end_of_track() && m[5].tick == 80);
    CHECK(merge_events({}, {}).empty());
    return 0;
}

}  // namespace

int main() {
    RUN(pairs_on_and_off);
    RUN(channels_are_separate);
    RUN(overlapping_same_pitch_is_first_in_first_out);
    RUN(nested_same_pitch_becomes_staggered);
    RUN(same_start_same_pitch_survives);
    RUN(back_to_back_notes_are_not_cut);
    RUN(zero_length_and_velocity_edges);
    RUN(unmatched_and_unterminated);
    RUN(unsorted_input_is_read_in_tick_order);
    RUN(many_notes_round_trip);
    RUN(merge_events_orders_and_keeps_one_end);
    std::printf("midi_notes: all tests passed\n");
    return 0;
}
