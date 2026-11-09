// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/chord.h"

namespace sonora::midi {

namespace {

struct Suffix {
    const char* text;
    Quality quality;
};

constexpr Suffix suffixes[] = {
    {"maj7", Quality::Major7},   {"sus2", Quality::Sus2},
    {"sus4", Quality::Sus4},     {"dim", Quality::Diminished},
    {"aug", Quality::Augmented}, {"m7", Quality::Minor7},
    {"m", Quality::Minor},       {"7", Quality::Dominant7},
    {"", Quality::Major},
};

const char* const sharp_names[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
const char* const flat_names[12] = {"C",  "Db", "D",  "Eb", "E",  "F",
                                    "Gb", "G",  "Ab", "A",  "Bb", "B"};

// Reads a note name at the start of `s`, advancing it.
std::optional<std::uint8_t> read_note(std::string_view& s) {
    if (s.empty()) return std::nullopt;
    int pc = 0;
    switch (s[0]) {
        case 'C': pc = 0; break;
        case 'D': pc = 2; break;
        case 'E': pc = 4; break;
        case 'F': pc = 5; break;
        case 'G': pc = 7; break;
        case 'A': pc = 9; break;
        case 'B': pc = 11; break;
        default: return std::nullopt;
    }
    s.remove_prefix(1);
    if (!s.empty() && s[0] == '#') {
        pc += 1;
        s.remove_prefix(1);
    } else if (!s.empty() && s[0] == 'b') {
        pc += 11;
        s.remove_prefix(1);
    }
    return static_cast<std::uint8_t>(pc % 12);
}

const char* quality_text(Quality q) {
    for (const Suffix& s : suffixes)
        if (s.quality == q) return s.text;
    return "";
}

}  // namespace

std::optional<Chord> parse_chord(std::string_view text) {
    std::string_view rest = text;
    const auto root = read_note(rest);
    if (!root) return std::nullopt;
    Chord chord;
    chord.root = *root;

    std::string_view qual = rest;
    std::string_view bass_text;
    const auto slash = rest.find('/');
    if (slash != std::string_view::npos) {
        qual = rest.substr(0, slash);
        bass_text = rest.substr(slash + 1);
    }
    bool found = false;
    for (const Suffix& s : suffixes) {
        if (qual == s.text) {
            chord.quality = s.quality;
            found = true;
            break;
        }
    }
    if (!found) return std::nullopt;
    if (slash != std::string_view::npos) {
        const auto bass = read_note(bass_text);
        if (!bass || !bass_text.empty()) return std::nullopt;
        chord.bass = *bass;
    }
    return chord;
}

std::string format_chord(const Chord& chord, bool use_flats) {
    const char* const* names = use_flats ? flat_names : sharp_names;
    std::string out = names[chord.root % 12];
    out += quality_text(chord.quality);
    if (chord.bass) {
        out += '/';
        out += names[*chord.bass % 12];
    }
    return out;
}

int chord_tones(const Chord& chord, std::uint8_t (&out)[4]) {
    int third = 4, fifth = 7, seventh = -1;
    switch (chord.quality) {
        case Quality::Major: break;
        case Quality::Minor: third = 3; break;
        case Quality::Diminished: third = 3; fifth = 6; break;
        case Quality::Augmented: fifth = 8; break;
        case Quality::Dominant7: seventh = 10; break;
        case Quality::Major7: seventh = 11; break;
        case Quality::Minor7: third = 3; seventh = 10; break;
        case Quality::Sus2: third = 2; break;
        case Quality::Sus4: third = 5; break;
    }
    out[0] = chord.root % 12;
    out[1] = static_cast<std::uint8_t>((chord.root + third) % 12);
    out[2] = static_cast<std::uint8_t>((chord.root + fifth) % 12);
    if (seventh < 0) return 3;
    out[3] = static_cast<std::uint8_t>((chord.root + seventh) % 12);
    return 4;
}

}  // namespace sonora::midi
