// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/chord.h"

#include "check.h"

using namespace sonora::midi;

namespace {

int round_trip() {
    const char* const names[] = {"C",     "Cm",    "F#dim", "Bbmaj7", "Am7", "G7",
                                 "Dsus4", "Esus2", "Gaug",  "C/E",    "Dm/F"};
    for (const char* s : names) {
        const auto c = parse_chord(s);
        CHECK(c.has_value());
        CHECK(parse_chord(format_chord(*c, false)) == c);
        CHECK(parse_chord(format_chord(*c, true)) == c);
    }
    return 0;
}

int sharps_and_flats() {
    const auto c = parse_chord("Bb7");
    CHECK(c && c->root == 10 && c->quality == Quality::Dominant7);
    CHECK(format_chord(*c, true) == "Bb7");
    CHECK(format_chord(*c, false) == "A#7");
    CHECK(parse_chord("A#7") == c);
    CHECK(parse_chord("Cb")->root == 11);
    return 0;
}

int slash_bass() {
    const auto c = parse_chord("C/E");
    CHECK(c && c->bass && *c->bass == 4);
    return 0;
}

int bad_input() {
    for (const char* s : {"", "H", "c", "Cx", "C/", "C/H", "C/E7", "Cm/", "#C", "C maj"})
        CHECK(!parse_chord(s).has_value());
    return 0;
}

}  // namespace

int main() {
    RUN(round_trip);
    RUN(sharps_and_flats);
    RUN(slash_bass);
    RUN(bad_input);
    return 0;
}
