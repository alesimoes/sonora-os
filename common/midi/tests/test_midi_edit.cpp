// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/edit.h"

#include "check.h"

using namespace sonora::midi;

namespace {

Note make(Tick start, Tick length, std::uint8_t pitch = 60, std::uint8_t velocity = 100) {
    Note n;
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = velocity;
    return n;
}

int quantize_full_strength() {
    QuantizeSettings q;
    q.grid = 240;
    const NoteList out = quantize({make(10, 100), make(130, 100), make(110, 50),
                                   make(470, 30)}, q);
    CHECK(out.size() == 4);
    // 110 is nearer 0 than 240; at one start the shorter note sorts first.
    CHECK(out[0].start == 0 && out[0].length == 50);
    CHECK(out[1].start == 0 && out[1].length == 100);
    CHECK(out[2].start == 240 && out[2].length == 100);
    CHECK(out[3].start == 480 && out[3].length == 30);
    return 0;
}

int quantize_strength_moves_part_way() {
    QuantizeSettings q;
    q.grid = 240;
    q.strength_percent = 50;
    const NoteList out = quantize({make(200, 10), make(100, 10)}, q);
    CHECK(out[0].start == 50);    // 100 -> 0, half way
    CHECK(out[1].start == 220);   // 200 -> 240, half way
    q.strength_percent = 0;
    CHECK(quantize({make(123, 10)}, q)[0].start == 123);
    q.strength_percent = 250;  // clamped to 100
    CHECK(quantize({make(123, 10)}, q)[0].start == 240);
    return 0;
}

int quantize_swing() {
    QuantizeSettings q;
    q.grid = 240;
    q.swing_percent = 100;  // triplet feel: the off-beat moves by grid/3
    CHECK(quantize({make(250, 10)}, q)[0].start == 320);
    CHECK(quantize({make(470, 10)}, q)[0].start == 480);  // on-beats stay put
    CHECK(quantize({make(700, 10)}, q)[0].start == 800);  // next pair's off-beat
    q.swing_percent = 50;
    CHECK(quantize({make(250, 10)}, q)[0].start == 280);
    q.swing_percent = 0;
    CHECK(quantize({make(250, 10)}, q)[0].start == 240);
    // An origin that is not 0: the grid is relative to it.
    q.origin = 100;
    CHECK(quantize({make(250, 10)}, q)[0].start == 340);
    return 0;
}

int quantize_on_and_length() {
    QuantizeSettings q;
    q.grid = 240;
    q.mode = QuantizeMode::NoteOnAndLength;
    const NoteList out = quantize({make(10, 450)}, q);
    CHECK(out[0].start == 0 && out[0].length == 480);
    // An end that would collapse onto the start gets a grid step.
    const NoteList tiny = quantize({make(230, 20)}, q);
    CHECK(tiny[0].start == 240 && tiny[0].length == 240);
    return 0;
}

int quantize_never_goes_negative_or_divides_by_zero() {
    QuantizeSettings q;
    q.grid = 240;
    q.origin = 200;
    CHECK(quantize({make(0, 10)}, q)[0].start == 0);  // nearest point is -40
    q.grid = 0;
    CHECK(quantize({make(17, 10)}, q)[0].start == 17);
    return 0;
}

int transpose_and_clamp() {
    const NoteList out = transpose({make(0, 10, 60), make(0, 10, 120), make(0, 10, 5)}, 12);
    CHECK(out[0].pitch == 17 && out[1].pitch == 72 && out[2].pitch == 127);
    CHECK(transpose({make(0, 10, 5)}, -12)[0].pitch == 0);
    CHECK(transpose({make(0, 10, 64)}, 0)[0].pitch == 64);
    return 0;
}

int velocity_edits() {
    const NoteList notes{make(0, 1, 60, 100), make(1, 1, 61, 10), make(2, 1, 62, 127)};
    const NoteList set = set_velocity(notes, 0);
    for (const Note& n : set) CHECK(n.velocity == 1);
    CHECK(set_velocity(notes, 90)[2].velocity == 90);

    const NoteList half = scale_velocity(notes, 50);
    CHECK(half[0].velocity == 50 && half[1].velocity == 5 && half[2].velocity == 64);
    const NoteList louder = scale_velocity(notes, 100, 20);
    CHECK(louder[0].velocity == 120 && louder[1].velocity == 30 && louder[2].velocity == 127);
    CHECK(scale_velocity(notes, 0)[0].velocity == 1);
    return 0;
}

int legato_fills_to_the_next_start() {
    const NoteList out = legato({make(0, 10, 60), make(0, 500, 64), make(480, 10, 62),
                                 make(960, 30, 65)});
    CHECK(out.size() == 4);
    CHECK(out[0].length == 480 && out[1].length == 480);  // a chord reaches the next note
    CHECK(out[2].start == 480 && out[2].length == 480);
    CHECK(out[3].length == 30);                            // the last keeps its length

    const NoteList gapped = legato({make(0, 10), make(100, 10)}, 20);
    CHECK(gapped[0].length == 80);
    const NoteList tight = legato({make(0, 10), make(5, 10)}, 20);
    CHECK(tight[0].length == 1);
    return 0;
}

int split_at_a_position() {
    const NoteList out = split_notes({make(0, 100, 60, 90), make(50, 10, 62), make(100, 50, 64)},
                                     50);
    CHECK(out.size() == 4);
    CHECK(out[0] == make(0, 50, 60, 90));
    CHECK(out[1] == make(50, 50, 60, 90));
    CHECK(out[2] == make(50, 10, 62));      // starts at the position: untouched
    CHECK(out[3] == make(100, 50, 64));
    // A note ending at the position is not split either.
    CHECK(split_notes({make(0, 50)}, 50).size() == 1);
    return 0;
}

int merge_keeps_everything_in_order() {
    const NoteList out = merge_notes({make(100, 10, 60), make(0, 10, 61)},
                                     {make(50, 10, 62), make(100, 10, 60)});
    CHECK(out.size() == 4);
    CHECK(out[0].start == 0 && out[1].start == 50 && out[2].start == 100 &&
          out[3].start == 100);
    CHECK(out[2] == out[3]);
    return 0;
}

int edits_do_not_touch_their_input() {
    const NoteList notes{make(130, 10, 60)};
    QuantizeSettings q;
    (void)quantize(notes, q);
    (void)transpose(notes, 5);
    (void)legato(notes);
    (void)split_notes(notes, 135);
    CHECK(notes[0] == make(130, 10, 60));
    return 0;
}

}  // namespace

int main() {
    RUN(quantize_full_strength);
    RUN(quantize_strength_moves_part_way);
    RUN(quantize_swing);
    RUN(quantize_on_and_length);
    RUN(quantize_never_goes_negative_or_divides_by_zero);
    RUN(transpose_and_clamp);
    RUN(velocity_edits);
    RUN(legato_fills_to_the_next_start);
    RUN(split_at_a_position);
    RUN(merge_keeps_everything_in_order);
    RUN(edits_do_not_touch_their_input);
    std::printf("midi_edit: all tests passed\n");
    return 0;
}
