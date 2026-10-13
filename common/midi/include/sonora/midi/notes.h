// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Notes as a piano roll sees them: one object per sounding note, with a
// length, instead of a note-on and a note-off somewhere later in the list.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonora/midi/event.h"

namespace sonora::midi {

struct Note {
    Tick start = 0;
    Tick length = 0;
    std::uint8_t channel = 0;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
    std::uint8_t release_velocity = default_release_velocity;

    Tick end() const { return start + length; }

    friend bool operator==(const Note& a, const Note& b) {
        return a.start == b.start && a.length == b.length &&
               a.channel == b.channel && a.pitch == b.pitch &&
               a.velocity == b.velocity &&
               a.release_velocity == b.release_velocity;
    }
    friend bool operator!=(const Note& a, const Note& b) { return !(a == b); }
};

using NoteList = std::vector<Note>;

struct NoteExtraction {
    NoteList notes;      // in sort_notes() order
    EventList other;     // every event that is not a note on or off
    // Note-offs with no sounding note of that pitch and channel to end.
    std::size_t unmatched_note_offs = 0;
    // Notes still sounding at the end of the list, ended there.
    std::size_t unterminated_notes = 0;
};

// Pairs note-ons with note-offs, per channel and pitch.
//
// Overlapping notes of the same pitch and channel pair first-in,
// first-out: the first note-off ends the earliest sounding note. A
// note-on with velocity 0 is a note-off. A note still sounding at the end
// is ended at the tick of the last event (the end of track, in a file).
//
// The input is processed in tick order; events at one tick keep their
// list order, so an off listed before an on at the same tick ends an
// earlier note, not the one starting there.
NoteExtraction extract_notes(const EventList& events);

// The inverse, sorted by tick. At one tick, note-offs come before
// note-ons, so a note that ends where the next of the same pitch starts is
// not cut short when re-read; a zero-length note's off follows its own on.
// A velocity of 0 is written as 1, since a note-on of 0 is a note-off.
//
// extract_notes(notes_to_events(n)) gives n back, in sort_notes() order,
// unless a note lies strictly inside another of the same pitch and
// channel. No pairing rule can carry that through a MIDI byte stream:
// under first-in, first-out the inner note's off ends the outer note, and
// the pair comes back as two staggered notes covering the same span.
EventList notes_to_events(const NoteList& notes);

// Stable merge by tick: at one tick, a's events come before b's. The end of
// track events collapse into one, last, at the later of the latest end of
// track and the last event.
EventList merge_events(const EventList& a, const EventList& b);

// The canonical order: start, then channel, pitch and end. Ending the
// shorter of two same-pitch notes that start together first is what lets
// them survive notes_to_events() and extract_notes().
void sort_notes(NoteList& notes);

}  // namespace sonora::midi
