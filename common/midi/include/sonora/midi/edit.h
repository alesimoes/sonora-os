// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI edit operations as the Pro Tools and Logic workflows use them
// (ADR-0004: we reproduce the behaviour, not anyone's code).
//
// Every function is pure: it takes the notes to operate on and returns a
// new list, in sort_notes() order. The caller passes only the selected
// notes and merges the result back; that keeps selection, which belongs
// to the timeline model, out of this library, and lets an undo step hold
// the before and after values as they are (timeline-model.md).

#include <cstdint>

#include "sonora/midi/notes.h"

namespace sonora::midi {

enum class QuantizeMode : std::uint8_t {
    // Move the note: the start is quantized and the length kept.
    NoteOn,
    // Quantize both edges: the start, and the end to the grid as well.
    NoteOnAndLength,
};

struct QuantizeSettings {
    Tick grid = 240;            // grid step in ticks (a 1/16 at 960 ppq)
    Tick origin = 0;            // a grid point, usually the region start
    int strength_percent = 100; // 0 leaves notes alone, 100 lands on the grid
    // Delays every second grid point. 0 is straight, 100 a triplet feel
    // (the off-beat lands two thirds of the way through the pair), the
    // Pro Tools convention; 300 would put it on the next beat, and the
    // range is clamped to 0..300.
    int swing_percent = 0;
    QuantizeMode mode = QuantizeMode::NoteOn;
};

// A note whose quantized end would not be after its quantized start gets
// one grid step of length rather than vanishing. A start would never go
// below 0. grid <= 0 returns the notes unchanged, sorted.
NoteList quantize(const NoteList& notes, const QuantizeSettings& settings);

// Pitches that would leave 0..127 are clamped to it. Transposing back then
// does not restore them; refusing the whole edit would be the alternative,
// and losing the octave of a few outliers is the smaller surprise.
NoteList transpose(const NoteList& notes, int semitones);

// Velocities stay in 1..127: 0 is a note-off on the wire.
NoteList set_velocity(const NoteList& notes, std::uint8_t velocity);
// velocity * percent / 100 + offset, rounded, then clamped.
NoteList scale_velocity(const NoteList& notes, int percent, int offset = 0);

// Each note is lengthened or shortened to end gap ticks before the next
// later start among the notes given, whatever its pitch, as a monophonic
// line or a chord progression wants. Notes of a chord all reach the next
// chord. The last start keeps its length. Never shorter than 1 tick.
NoteList legato(const NoteList& notes, Tick gap = 0);

// Every note with start < position < end becomes two notes that meet at
// position, both with the original velocities. Others are unchanged.
NoteList split_notes(const NoteList& notes, Tick position);

// Both lists in one, sorted. Identical notes are all kept: dropping one
// would be a guess about which edit the user meant.
NoteList merge_notes(const NoteList& a, const NoteList& b);

}  // namespace sonora::midi
