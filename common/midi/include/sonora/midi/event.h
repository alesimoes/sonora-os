// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI 1.0 events in musical time, as the Standard MIDI File stores them.
//
// Not for the audio thread: an Event owns a vector for SysEx and meta data.
// The audio thread gets raw bytes through sonora::MidiBuffer (common/base).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sonora::midi {

// Ticks from the start of the track, at the file's ticks-per-quarter.
// Signed so that edit arithmetic can go negative before it is clamped.
using Tick = std::int64_t;

namespace status {
constexpr std::uint8_t note_off = 0x80;
constexpr std::uint8_t note_on = 0x90;
constexpr std::uint8_t poly_aftertouch = 0xA0;
constexpr std::uint8_t control_change = 0xB0;
constexpr std::uint8_t program_change = 0xC0;
constexpr std::uint8_t channel_aftertouch = 0xD0;
constexpr std::uint8_t pitch_bend = 0xE0;
constexpr std::uint8_t sysex = 0xF0;
// SMF only: a SysEx continuation packet, or raw bytes sent as they are.
constexpr std::uint8_t sysex_escape = 0xF7;
// SMF only. On the wire 0xFF is System Reset; a file reuses it for meta.
constexpr std::uint8_t meta = 0xFF;
}  // namespace status

namespace meta {
constexpr std::uint8_t sequence_number = 0x00;
constexpr std::uint8_t text = 0x01;
constexpr std::uint8_t copyright = 0x02;
constexpr std::uint8_t track_name = 0x03;
constexpr std::uint8_t instrument_name = 0x04;
constexpr std::uint8_t lyric = 0x05;
constexpr std::uint8_t marker = 0x06;
constexpr std::uint8_t cue_point = 0x07;
constexpr std::uint8_t channel_prefix = 0x20;
constexpr std::uint8_t end_of_track = 0x2F;
constexpr std::uint8_t tempo = 0x51;
constexpr std::uint8_t smpte_offset = 0x54;
constexpr std::uint8_t time_signature = 0x58;
constexpr std::uint8_t key_signature = 0x59;
constexpr std::uint8_t sequencer_specific = 0x7F;
}  // namespace meta

enum class Kind : std::uint8_t {
    NoteOff,
    NoteOn,
    PolyAftertouch,
    ControlChange,
    ProgramChange,
    ChannelAftertouch,
    PitchBend,
    SysEx,
    SysExEscape,
    Meta,
    Invalid,
};

// The MIDI 1.0 default for a note-off from a keyboard without release
// velocity sensing.
constexpr std::uint8_t default_release_velocity = 64;

// 500,000 us per quarter note, 120 BPM: the SMF default until a tempo event.
constexpr std::uint32_t default_usec_per_quarter = 500000;

struct Event {
    Tick tick = 0;
    // Channel messages carry their channel in the low nibble. Otherwise one
    // of status::sysex, status::sysex_escape, status::meta.
    std::uint8_t status = 0;
    // Channel messages: the data bytes. Meta: data1 is the meta type.
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    // SysEx: the bytes after 0xF0, with the closing 0xF7 if the message is
    // complete. Escape: the raw bytes. Meta: the meta data.
    std::vector<std::uint8_t> payload;

    Kind kind() const;
    bool is_channel() const { return status >= 0x80 && status < 0xF0; }
    std::uint8_t channel() const { return status & 0x0F; }

    // A note-on with velocity 0 is a note-off (MIDI 1.0), so these two
    // never both hold and every note message satisfies exactly one.
    bool is_note_on() const;
    bool is_note_off() const;
    std::uint8_t note() const { return data1; }
    std::uint8_t velocity() const { return data2; }

    bool is_meta(std::uint8_t type) const {
        return status == status::meta && data1 == type;
    }
    bool is_end_of_track() const { return is_meta(meta::end_of_track); }

    // Pitch bend as a signed offset from centre, -8192..8191.
    int pitch_bend_value() const;

    friend bool operator==(const Event& a, const Event& b) {
        return a.tick == b.tick && a.status == b.status && a.data1 == b.data1 &&
               a.data2 == b.data2 && a.payload == b.payload;
    }
    friend bool operator!=(const Event& a, const Event& b) { return !(a == b); }
};

using EventList = std::vector<Event>;

// Factories. Data values above 127 are clamped to 127 and channels taken
// modulo 16: a 7-bit field cannot hold more, and wrapping a velocity of
// 130 to 2 would be worse than pinning it.
Event note_on(Tick tick, std::uint8_t channel, std::uint8_t note,
              std::uint8_t velocity);
Event note_off(Tick tick, std::uint8_t channel, std::uint8_t note,
               std::uint8_t release_velocity = default_release_velocity);
Event poly_aftertouch(Tick tick, std::uint8_t channel, std::uint8_t note,
                      std::uint8_t pressure);
Event control_change(Tick tick, std::uint8_t channel, std::uint8_t controller,
                     std::uint8_t value);
Event program_change(Tick tick, std::uint8_t channel, std::uint8_t program);
Event channel_aftertouch(Tick tick, std::uint8_t channel, std::uint8_t pressure);
// value is -8192..8191 and clamped to it.
Event pitch_bend(Tick tick, std::uint8_t channel, int value);

// A 14-bit controller as the MIDI 1.0 pair: MSB on controller 0..31, then
// LSB on controller + 32. The MSB goes first because a receiver resets the
// LSB to zero when the MSB arrives.
std::array<Event, 2> control_change_14bit(Tick tick, std::uint8_t channel,
                                          std::uint8_t controller,
                                          std::uint16_t value);

// body is the message without the leading 0xF0; a closing 0xF7 is added if
// it is missing.
Event sysex(Tick tick, std::vector<std::uint8_t> body);
Event meta_event(Tick tick, std::uint8_t type, std::vector<std::uint8_t> data);
Event tempo(Tick tick, std::uint32_t usec_per_quarter);
// denominator is the written note value (4 for x/4) and must be a power of
// two from 1 to 128; anything else is rounded down to one.
Event time_signature(Tick tick, std::uint8_t numerator, std::uint8_t denominator,
                     std::uint8_t clocks_per_click = 24,
                     std::uint8_t thirty_seconds_per_quarter = 8);
// sharps_flats: -7 (7 flats) .. 7 (7 sharps).
Event key_signature(Tick tick, int sharps_flats, bool minor);
Event text_event(Tick tick, std::uint8_t type, const std::string& text);
Event marker(Tick tick, const std::string& text);
Event track_name(Tick tick, const std::string& text);
Event end_of_track(Tick tick);

struct TimeSignature {
    std::uint8_t numerator = 4;
    std::uint8_t denominator = 4;
    std::uint8_t clocks_per_click = 24;
    std::uint8_t thirty_seconds_per_quarter = 8;
};

struct KeySignature {
    int sharps_flats = 0;
    bool minor = false;
};

// Decoders; empty when the event is not of that kind or is malformed.
std::optional<std::uint32_t> tempo_of(const Event& e);
std::optional<TimeSignature> time_signature_of(const Event& e);
std::optional<KeySignature> key_signature_of(const Event& e);
// Text, marker, track name and the other 0x01..0x0F meta types.
std::optional<std::string> text_of(const Event& e);

// A 14-bit controller value, as a receiver would hold it after an event.
struct Cc14Value {
    Tick tick = 0;
    std::uint8_t channel = 0;
    std::uint8_t controller = 0;  // 0..31, the MSB controller
    std::uint16_t value = 0;      // 0..16383
};

// Follows the receiver rules of MIDI 1.0: an MSB sets the high 7 bits and
// resets the LSB to 0; an LSB sets the low 7 bits of the last MSB. One value
// per MSB or LSB event, in input order.
std::vector<Cc14Value> decode_cc14(const EventList& events);

// Stable sort by tick: events at one tick keep their order, because that
// order means something (program change before the note it selects).
void sort_by_tick(EventList& events);

}  // namespace sonora::midi
