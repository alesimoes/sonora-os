// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Makes a note list follow a chord track (ADR-0126). Control thread only.

#include <cstdint>

#include "sonora/midi/chord.h"
#include "sonora/midi/chord_track.h"
#include "sonora/midi/notes.h"

namespace sonora::midi {

enum class FollowMode : std::uint8_t {
    // Shift each note by the shortest root interval (-6..+5 semitones) from
    // the reference chord to the chord at the note's start.
    Transpose,
    // Move each note to the nearest tone of the chord at its start (ties go
    // down); the reference chord is not used.
    SnapToChord,
};

// Returns a new list; `notes` is not modified. Notes starting before the
// first chord, and every note when the track is empty, are unchanged. A
// pitch that would leave 0-127 is moved by whole octaves back into range.
NoteList follow_chords(const NoteList& notes, const ChordTrack& track,
                       const Chord& reference, FollowMode mode);

}  // namespace sonora::midi
