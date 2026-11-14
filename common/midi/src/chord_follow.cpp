// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/chord_follow.h"

namespace sonora::midi {

namespace {

// Octave shifts keep the pitch class and bring the pitch into 0-127.
int clamp_by_octave(int pitch) {
    while (pitch < 0) pitch += 12;
    while (pitch > 127) pitch -= 12;
    return pitch;
}

// Shortest signed interval from `from` to `to`, in -6..+5.
int root_interval(int from, int to) {
    const int d = ((to - from) % 12 + 12) % 12;
    return d > 5 ? d - 12 : d;
}

int pitch_class(int pitch) { return ((pitch % 12) + 12) % 12; }

// Nearest chord tone; at equal distance the lower one wins.
int snap_pitch(int pitch, const Chord& chord) {
    std::uint8_t tones[4];
    const int n = chord_tones(chord, tones);
    for (int dist = 0; dist <= 6; ++dist) {
        for (int i = 0; i < n; ++i)
            if (pitch_class(pitch - dist) == tones[i]) return pitch - dist;
        for (int i = 0; i < n; ++i)
            if (pitch_class(pitch + dist) == tones[i]) return pitch + dist;
    }
    return pitch;
}

}  // namespace

NoteList follow_chords(const NoteList& notes, const ChordTrack& track,
                       const Chord& reference, FollowMode mode) {
    NoteList out = notes;
    for (Note& n : out) {
        const auto chord = track.chord_at(n.start);
        if (!chord) continue;
        int pitch = n.pitch;
        if (mode == FollowMode::Transpose)
            pitch += root_interval(reference.root, chord->root);
        else
            pitch = snap_pitch(pitch, *chord);
        n.pitch = static_cast<std::uint8_t>(clamp_by_octave(pitch));
    }
    return out;
}

}  // namespace sonora::midi
