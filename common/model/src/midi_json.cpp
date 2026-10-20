// SPDX-License-Identifier: GPL-3.0-or-later
#include "midi_json.h"

#include <cstdint>

namespace sonora::model::detail {

namespace {

using json::Value;

Value integer(std::int64_t n) { return Value::integer(n); }

bool fail(RowError& error, const Value& at, std::string where, std::string message) {
    error.message = std::move(message);
    error.where = std::move(where);
    error.at = &at;
    return false;
}

std::string index(std::size_t i) { return "[" + std::to_string(i) + "]"; }

bool cell(const Value& r, std::size_t i, std::size_t k, std::int64_t max, RowError& error,
          std::int64_t& out) {
    const Value& v = r.items()[k];
    if (!v.is_number() || !v.is_integer_lexeme() || !v.to_int64(out) || out < 0 || out > max) {
        return fail(error, v, index(i) + index(k),
                    "must be an integer from 0 to " + std::to_string(max));
    }
    return true;
}

const char kHex[] = "0123456789abcdef";

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// One row's values from column `at` on; row `i` of its list, for paths.
bool note_at(const Value& r, std::size_t i, std::size_t at, midi::Note& n, RowError& error) {
    std::int64_t c[6] = {};
    for (std::size_t k = 0; k < 6; ++k) {
        if (!cell(r, i, at + k, k < 2 ? kMaxInteger : 255, error, c[k])) return false;
    }
    n.start = c[0];
    n.length = c[1];
    n.channel = static_cast<std::uint8_t>(c[2]);
    n.pitch = static_cast<std::uint8_t>(c[3]);
    n.velocity = static_cast<std::uint8_t>(c[4]);
    n.release_velocity = static_cast<std::uint8_t>(c[5]);
    return true;
}

bool event_at(const Value& r, std::size_t i, std::size_t at, MidiEvent& e, RowError& error) {
    std::int64_t c[4] = {};
    for (std::size_t k = 0; k < 4; ++k) {
        if (!cell(r, i, at + k, k == 0 ? kMaxInteger : 255, error, c[k])) return false;
    }
    e = MidiEvent{c[0], static_cast<std::uint8_t>(c[1]), static_cast<std::uint8_t>(c[2]),
                  static_cast<std::uint8_t>(c[3])};
    return true;
}

bool sysex_at(const Value& r, std::size_t i, std::size_t at, MidiSysEx& x, RowError& error) {
    if (!cell(r, i, at, kMaxInteger, error, x.tick)) return false;
    const Value& h = r.items()[at + 1];
    // Checked before decoding, so a hostile file cannot make a large buffer
    // out of one string.
    if (!h.is_string() || h.text().size() % 2 != 0 || h.text().size() > 2 * kMaxSysexBytes) {
        return fail(error, h, index(i) + index(at + 1),
                    "must be the message in lower-case hex, at most " +
                        std::to_string(kMaxSysexBytes) + " bytes");
    }
    x.bytes.clear();
    x.bytes.reserve(h.text().size() / 2);
    for (std::size_t k = 0; k < h.text().size(); k += 2) {
        const int hi = hex_digit(h.text()[k]);
        const int lo = hex_digit(h.text()[k + 1]);
        if (hi < 0 || lo < 0) {
            return fail(error, h, index(i) + index(at + 1), "must be lower-case hex digits");
        }
        x.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return true;
}

// Every row of `list`: `width` values after an index column when `indexed`.
template <typename T, typename Parse>
bool rows(const Value& list, std::size_t width, bool indexed, const char* shape, Parse parse,
          std::vector<T>& out, RowError& error) {
    if (!list.is_array()) return fail(error, list, "", "must be an array");
    out.reserve(list.items().size());
    const std::size_t at = indexed ? 1 : 0;
    for (std::size_t i = 0; i < list.items().size(); ++i) {
        const Value& r = list.items()[i];
        if (!r.is_array() || r.items().size() != width + at) {
            return fail(error, r, index(i),
                        std::string("must be ") + (indexed ? "[index, " : "[") + shape + "]");
        }
        T value;
        if (!parse(r, i, at, value, error)) return false;
        out.push_back(std::move(value));
    }
    return true;
}

const char kNoteShape[] = "start, length, channel, pitch, velocity, release_velocity";
const char kEventShape[] = "tick, status, data1, data2";
const char kSysExShape[] = "tick, \"f0...f7\"";

// The removal's index column; the value is parsed by `parse` after it.
template <typename R, typename V, typename Parse>
bool indexed(const Value& r, std::size_t i, R& out, V& value, Parse parse, RowError& error) {
    std::int64_t n = 0;
    if (!cell(r, i, 0, 0xFFFFFFFF, error, n)) return false;
    out.index = static_cast<std::uint32_t>(n);
    return parse(r, i, 1, value, error);
}

void push_hex(const std::vector<std::uint8_t>& bytes, Value& row) {
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0x0F]);
    }
    row.push(Value::string(std::move(hex)));
}

void push_note(const midi::Note& n, Value& v) {
    v.push(integer(n.start));
    v.push(integer(n.length));
    v.push(integer(n.channel));
    v.push(integer(n.pitch));
    v.push(integer(n.velocity));
    v.push(integer(n.release_velocity));
}

void push_event(const MidiEvent& e, Value& v) {
    v.push(integer(e.tick));
    v.push(integer(e.status));
    v.push(integer(e.data1));
    v.push(integer(e.data2));
}

}  // namespace

Value note_row(const midi::Note& n) {
    Value v = Value::array();
    push_note(n, v);
    return v;
}

Value event_row(const MidiEvent& e) {
    Value v = Value::array();
    push_event(e, v);
    return v;
}

Value sysex_row(const MidiSysEx& x) {
    Value v = Value::array();
    v.push(integer(x.tick));
    push_hex(x.bytes, v);
    return v;
}

Value note_removal_row(const NoteRemoval& r) {
    Value v = Value::array();
    v.push(integer(r.index));
    push_note(r.note, v);
    return v;
}

Value event_removal_row(const EventRemoval& r) {
    Value v = Value::array();
    v.push(integer(r.index));
    push_event(r.event, v);
    return v;
}

Value sysex_removal_row(const SysExRemoval& r) {
    Value v = Value::array();
    v.push(integer(r.index));
    v.push(integer(r.sysex.tick));
    push_hex(r.sysex.bytes, v);
    return v;
}

bool read_note_rows(const Value& list, std::vector<midi::Note>& out, RowError& error) {
    return rows(list, 6, false, kNoteShape, note_at, out, error);
}

bool read_event_rows(const Value& list, std::vector<MidiEvent>& out, RowError& error) {
    return rows(list, 4, false, kEventShape, event_at, out, error);
}

bool read_sysex_rows(const Value& list, std::vector<MidiSysEx>& out, RowError& error) {
    return rows(list, 2, false, kSysExShape, sysex_at, out, error);
}

bool read_note_removal_rows(const Value& list, std::vector<NoteRemoval>& out, RowError& error) {
    const auto parse = [](const Value& r, std::size_t i, std::size_t, NoteRemoval& o,
                          RowError& e) { return indexed(r, i, o, o.note, note_at, e); };
    return rows(list, 6, true, kNoteShape, parse, out, error);
}

bool read_event_removal_rows(const Value& list, std::vector<EventRemoval>& out,
                             RowError& error) {
    const auto parse = [](const Value& r, std::size_t i, std::size_t, EventRemoval& o,
                          RowError& e) { return indexed(r, i, o, o.event, event_at, e); };
    return rows(list, 4, true, kEventShape, parse, out, error);
}

bool read_sysex_removal_rows(const Value& list, std::vector<SysExRemoval>& out,
                             RowError& error) {
    const auto parse = [](const Value& r, std::size_t i, std::size_t, SysExRemoval& o,
                          RowError& e) { return indexed(r, i, o, o.sysex, sysex_at, e); };
    return rows(list, 2, true, kSysExShape, parse, out, error);
}

}  // namespace sonora::model::detail
