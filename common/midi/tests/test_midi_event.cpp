// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/event.h"

#include "check.h"

using namespace sonora::midi;

namespace {

int note_messages() {
    const Event on = note_on(10, 3, 60, 100);
    CHECK(on.status == 0x93 && on.channel() == 3 && on.kind() == Kind::NoteOn);
    CHECK(on.is_note_on() && !on.is_note_off());
    CHECK(on.note() == 60 && on.velocity() == 100);

    // Release velocity is the note-off's second data byte.
    const Event off = note_off(20, 3, 60, 37);
    CHECK(off.status == 0x83 && off.is_note_off() && !off.is_note_on());
    CHECK(off.velocity() == 37);
    CHECK(note_off(0, 0, 60).velocity() == default_release_velocity);

    // A note-on of velocity 0 is a note-off (MIDI 1.0).
    const Event zero = note_on(0, 0, 60, 0);
    CHECK(zero.kind() == Kind::NoteOn && zero.is_note_off() && !zero.is_note_on());

    // Out-of-range data clamps, channels wrap.
    const Event loud = note_on(0, 17, 200, 255);
    CHECK(loud.channel() == 1 && loud.note() == 127 && loud.velocity() == 127);
    return 0;
}

int channel_messages() {
    CHECK(poly_aftertouch(0, 1, 64, 90).status == 0xA1);
    CHECK(poly_aftertouch(0, 1, 64, 90).kind() == Kind::PolyAftertouch);
    CHECK(control_change(0, 2, 7, 100).status == 0xB2);
    CHECK(program_change(0, 9, 5).status == 0xC9);
    CHECK(program_change(0, 9, 5).kind() == Kind::ProgramChange);
    CHECK(channel_aftertouch(0, 0, 80).kind() == Kind::ChannelAftertouch);

    // Pitch bend: centre is 0x2000, LSB first on the wire.
    const Event centre = pitch_bend(0, 0, 0);
    CHECK(centre.data1 == 0x00 && centre.data2 == 0x40);
    CHECK(centre.pitch_bend_value() == 0);
    CHECK(pitch_bend(0, 0, -8192).pitch_bend_value() == -8192);
    CHECK(pitch_bend(0, 0, 8191).pitch_bend_value() == 8191);
    CHECK(pitch_bend(0, 0, 100000).pitch_bend_value() == 8191);
    CHECK(pitch_bend(0, 0, 1234).pitch_bend_value() == 1234);
    CHECK(pitch_bend(0, 0, 0).kind() == Kind::PitchBend);
    return 0;
}

int fourteen_bit_controllers() {
    const auto pair = control_change_14bit(0, 4, 1, 0x1234);
    CHECK(pair[0].data1 == 1 && pair[0].data2 == (0x1234 >> 7));
    CHECK(pair[1].data1 == 33 && pair[1].data2 == (0x1234 & 0x7F));
    CHECK(pair[0].channel() == 4 && pair[1].channel() == 4);

    EventList events{pair[0], pair[1]};
    // A fine adjustment: LSB alone keeps the MSB.
    events.push_back(control_change(5, 4, 33, 0x7F));
    // A new MSB resets the LSB to 0.
    events.push_back(control_change(6, 4, 1, 0x10));
    // Another channel and controller are independent.
    events.push_back(control_change(7, 5, 39, 0x05));
    // Not a 14-bit controller: ignored.
    events.push_back(control_change(8, 4, 64, 127));

    const auto values = decode_cc14(events);
    CHECK(values.size() == 5);
    CHECK(values[0].value == ((0x1234 >> 7) << 7));
    CHECK(values[1].value == 0x1234);
    CHECK(values[2].value == (((0x1234 >> 7) << 7) | 0x7F) && values[2].tick == 5);
    CHECK(values[3].value == (0x10 << 7));
    CHECK(values[4].channel == 5 && values[4].controller == 7 && values[4].value == 0x05);
    return 0;
}

int sysex_and_meta() {
    const Event sx = sysex(0, {0x7E, 0x7F, 0x09, 0x01});
    CHECK(sx.kind() == Kind::SysEx && sx.payload.size() == 5 && sx.payload.back() == 0xF7);
    CHECK(sysex(0, {0x41, 0xF7}).payload.size() == 2);

    const Event t = tempo(0, 500000);
    CHECK(t.kind() == Kind::Meta && t.is_meta(meta::tempo));
    CHECK(t.payload == std::vector<std::uint8_t>({0x07, 0xA1, 0x20}));
    CHECK(tempo_of(t) && *tempo_of(t) == 500000);
    CHECK(!tempo_of(note_on(0, 0, 1, 1)));

    const Event ts = time_signature(0, 6, 8);
    CHECK(ts.payload == std::vector<std::uint8_t>({6, 3, 24, 8}));
    const auto decoded = time_signature_of(ts);
    CHECK(decoded && decoded->numerator == 6 && decoded->denominator == 8);
    CHECK(time_signature_of(time_signature(0, 4, 4))->denominator == 4);
    CHECK(time_signature_of(time_signature(0, 3, 1))->denominator == 1);
    CHECK(time_signature_of(time_signature(0, 5, 128))->denominator == 128);
    CHECK(time_signature_of(time_signature(0, 5, 6))->denominator == 4);

    const auto ks = key_signature_of(key_signature(0, -3, true));
    CHECK(ks && ks->sharps_flats == -3 && ks->minor);
    CHECK(key_signature(0, -3, true).payload[0] == 0xFD);
    CHECK(key_signature_of(key_signature(0, 9, false))->sharps_flats == 7);

    CHECK(text_of(marker(0, "Chorus")) == std::string("Chorus"));
    CHECK(text_of(track_name(0, "Bass")) == std::string("Bass"));
    CHECK(marker(0, "x").is_meta(meta::marker));
    CHECK(!text_of(tempo(0, 1)));

    const Event eot = end_of_track(99);
    CHECK(eot.is_end_of_track() && eot.payload.empty() && eot.tick == 99);
    return 0;
}

int sort_is_stable() {
    EventList events{note_on(10, 0, 1, 1), program_change(5, 0, 2),
                     control_change(10, 0, 7, 1), note_on(5, 0, 3, 1)};
    sort_by_tick(events);
    CHECK(events[0].kind() == Kind::ProgramChange);
    CHECK(events[1].note() == 3);
    CHECK(events[2].kind() == Kind::NoteOn && events[2].note() == 1);
    CHECK(events[3].kind() == Kind::ControlChange);
    return 0;
}

}  // namespace

int main() {
    RUN(note_messages);
    RUN(channel_messages);
    RUN(fourteen_bit_controllers);
    RUN(sysex_and_meta);
    RUN(sort_is_stable);
    std::printf("midi_event: all tests passed\n");
    return 0;
}
