// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/event.h"

#include <algorithm>
#include <utility>

namespace sonora::midi {

namespace {

std::uint8_t clamp7(std::uint8_t v) { return v > 127 ? 127 : v; }

Event channel_event(Tick tick, std::uint8_t kind, std::uint8_t channel,
                    std::uint8_t d1, std::uint8_t d2) {
    Event e;
    e.tick = tick;
    e.status = static_cast<std::uint8_t>(kind | (channel & 0x0F));
    e.data1 = clamp7(d1);
    e.data2 = clamp7(d2);
    return e;
}

}  // namespace

Kind Event::kind() const {
    switch (status & 0xF0) {
        case status::note_off: return Kind::NoteOff;
        case status::note_on: return Kind::NoteOn;
        case status::poly_aftertouch: return Kind::PolyAftertouch;
        case status::control_change: return Kind::ControlChange;
        case status::program_change: return Kind::ProgramChange;
        case status::channel_aftertouch: return Kind::ChannelAftertouch;
        case status::pitch_bend: return Kind::PitchBend;
        default: break;
    }
    switch (status) {
        case status::sysex: return Kind::SysEx;
        case status::sysex_escape: return Kind::SysExEscape;
        case status::meta: return Kind::Meta;
        default: return Kind::Invalid;
    }
}

bool Event::is_note_on() const {
    return (status & 0xF0) == status::note_on && data2 > 0;
}

bool Event::is_note_off() const {
    return (status & 0xF0) == status::note_off ||
           ((status & 0xF0) == status::note_on && data2 == 0);
}

int Event::pitch_bend_value() const {
    return ((static_cast<int>(data2) << 7) | data1) - 8192;
}

Event note_on(Tick tick, std::uint8_t channel, std::uint8_t note,
              std::uint8_t velocity) {
    return channel_event(tick, status::note_on, channel, note, velocity);
}

Event note_off(Tick tick, std::uint8_t channel, std::uint8_t note,
               std::uint8_t release_velocity) {
    return channel_event(tick, status::note_off, channel, note, release_velocity);
}

Event poly_aftertouch(Tick tick, std::uint8_t channel, std::uint8_t note,
                      std::uint8_t pressure) {
    return channel_event(tick, status::poly_aftertouch, channel, note, pressure);
}

Event control_change(Tick tick, std::uint8_t channel, std::uint8_t controller,
                     std::uint8_t value) {
    return channel_event(tick, status::control_change, channel, controller, value);
}

Event program_change(Tick tick, std::uint8_t channel, std::uint8_t program) {
    return channel_event(tick, status::program_change, channel, program, 0);
}

Event channel_aftertouch(Tick tick, std::uint8_t channel, std::uint8_t pressure) {
    return channel_event(tick, status::channel_aftertouch, channel, pressure, 0);
}

Event pitch_bend(Tick tick, std::uint8_t channel, int value) {
    const int raw = std::clamp(value, -8192, 8191) + 8192;
    return channel_event(tick, status::pitch_bend, channel,
                         static_cast<std::uint8_t>(raw & 0x7F),
                         static_cast<std::uint8_t>(raw >> 7));
}

std::array<Event, 2> control_change_14bit(Tick tick, std::uint8_t channel,
                                          std::uint8_t controller,
                                          std::uint16_t value) {
    const std::uint8_t cc = controller & 0x1F;
    const std::uint16_t v = value > 16383 ? 16383 : value;
    return {control_change(tick, channel, cc, static_cast<std::uint8_t>(v >> 7)),
            control_change(tick, channel, static_cast<std::uint8_t>(cc + 32),
                           static_cast<std::uint8_t>(v & 0x7F))};
}

Event sysex(Tick tick, std::vector<std::uint8_t> body) {
    if (body.empty() || body.back() != 0xF7) body.push_back(0xF7);
    Event e;
    e.tick = tick;
    e.status = status::sysex;
    e.payload = std::move(body);
    return e;
}

Event meta_event(Tick tick, std::uint8_t type, std::vector<std::uint8_t> data) {
    Event e;
    e.tick = tick;
    e.status = status::meta;
    e.data1 = clamp7(type);
    e.payload = std::move(data);
    return e;
}

Event tempo(Tick tick, std::uint32_t usec_per_quarter) {
    const std::uint32_t t = std::clamp<std::uint32_t>(usec_per_quarter, 1, 0xFFFFFF);
    return meta_event(tick, meta::tempo,
                      {static_cast<std::uint8_t>(t >> 16),
                       static_cast<std::uint8_t>(t >> 8),
                       static_cast<std::uint8_t>(t)});
}

Event time_signature(Tick tick, std::uint8_t numerator, std::uint8_t denominator,
                     std::uint8_t clocks_per_click,
                     std::uint8_t thirty_seconds_per_quarter) {
    std::uint8_t power = 0;
    while (power < 7 && (2u << power) <= denominator) ++power;
    return meta_event(tick, meta::time_signature,
                      {numerator, power, clocks_per_click,
                       thirty_seconds_per_quarter});
}

Event key_signature(Tick tick, int sharps_flats, bool minor) {
    const int sf = std::clamp(sharps_flats, -7, 7);
    return meta_event(tick, meta::key_signature,
                      {static_cast<std::uint8_t>(static_cast<std::int8_t>(sf)),
                       static_cast<std::uint8_t>(minor ? 1 : 0)});
}

Event text_event(Tick tick, std::uint8_t type, const std::string& text) {
    return meta_event(tick, type, std::vector<std::uint8_t>(text.begin(), text.end()));
}

Event marker(Tick tick, const std::string& text) {
    return text_event(tick, meta::marker, text);
}

Event track_name(Tick tick, const std::string& text) {
    return text_event(tick, meta::track_name, text);
}

Event end_of_track(Tick tick) { return meta_event(tick, meta::end_of_track, {}); }

std::optional<std::uint32_t> tempo_of(const Event& e) {
    if (!e.is_meta(meta::tempo) || e.payload.size() != 3) return std::nullopt;
    const std::uint32_t t = (std::uint32_t{e.payload[0]} << 16) |
                            (std::uint32_t{e.payload[1]} << 8) | e.payload[2];
    if (t == 0) return std::nullopt;
    return t;
}

std::optional<TimeSignature> time_signature_of(const Event& e) {
    if (!e.is_meta(meta::time_signature) || e.payload.size() != 4 ||
        e.payload[1] > 7) {
        return std::nullopt;
    }
    TimeSignature ts;
    ts.numerator = e.payload[0];
    ts.denominator = static_cast<std::uint8_t>(1u << e.payload[1]);
    ts.clocks_per_click = e.payload[2];
    ts.thirty_seconds_per_quarter = e.payload[3];
    return ts;
}

std::optional<KeySignature> key_signature_of(const Event& e) {
    if (!e.is_meta(meta::key_signature) || e.payload.size() != 2) {
        return std::nullopt;
    }
    const int sf = static_cast<std::int8_t>(e.payload[0]);
    if (sf < -7 || sf > 7 || e.payload[1] > 1) return std::nullopt;
    return KeySignature{sf, e.payload[1] == 1};
}

std::optional<std::string> text_of(const Event& e) {
    if (e.status != status::meta || e.data1 < 0x01 || e.data1 > 0x0F) {
        return std::nullopt;
    }
    return std::string(e.payload.begin(), e.payload.end());
}

std::vector<Cc14Value> decode_cc14(const EventList& events) {
    // Per channel and MSB controller, the value the receiver now holds.
    std::array<std::array<std::uint16_t, 32>, 16> held{};
    std::vector<Cc14Value> out;
    for (const Event& e : events) {
        if (e.kind() != Kind::ControlChange || e.data1 >= 64) continue;
        const std::uint8_t ch = e.channel();
        const std::uint8_t cc = e.data1 & 0x1F;
        std::uint16_t& v = held[ch][cc];
        if (e.data1 < 32) {
            v = static_cast<std::uint16_t>(e.data2 << 7);
        } else {
            v = static_cast<std::uint16_t>((v & 0x3F80) | e.data2);
        }
        out.push_back(Cc14Value{e.tick, ch, cc, v});
    }
    return out;
}

void sort_by_tick(EventList& events) {
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return a.tick < b.tick; });
}

}  // namespace sonora::midi
