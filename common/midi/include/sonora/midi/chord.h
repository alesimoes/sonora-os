// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A chord symbol: root pitch class, quality, optional slash bass. Control
// thread only (ADR-0126); strings allocate.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sonora::midi {

enum class Quality : std::uint8_t {
    Major,
    Minor,
    Diminished,
    Augmented,
    Dominant7,
    Major7,
    Minor7,
    Sus2,
    Sus4,
};

struct Chord {
    std::uint8_t root = 0;  // pitch class 0-11, C = 0
    Quality quality = Quality::Major;
    std::optional<std::uint8_t> bass;  // pitch class of a slash bass

    friend bool operator==(const Chord& a, const Chord& b) {
        return a.root == b.root && a.quality == b.quality && a.bass == b.bass;
    }
    friend bool operator!=(const Chord& a, const Chord& b) { return !(a == b); }
};

// "C", "Cm", "F#dim", "Bbmaj7", "Am7", "G7", "Dsus4", "C/E". Empty on bad input.
std::optional<Chord> parse_chord(std::string_view text);

std::string format_chord(const Chord& chord, bool use_flats = false);

// Pitch classes (0-11) of the chord's tones, root first; bass not included.
// Fills `out` and returns the count (3 or 4).
int chord_tones(const Chord& chord, std::uint8_t (&out)[4]);

}  // namespace sonora::midi
