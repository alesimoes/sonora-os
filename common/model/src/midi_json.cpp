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

// The row `i` of `list`, which must be an array of `n` values.
bool row(const Value& list, std::size_t i, std::size_t n, const char* shape, RowError& error,
         const Value*& out) {
    const Value& r = list.items()[i];
    if (!r.is_array() || r.items().size() != n) {
        return fail(error, r, index(i), std::string("must be ") + shape);
    }
    out = &r;
    return true;
}

bool cell(const Value& r, std::size_t i, std::size_t k, std::int64_t max, RowError& error,
          std::int64_t& out) {
    const Value& v = r.items()[k];
    if (!v.is_number() || !v.is_integer_lexeme() || !v.to_int64(out) || out < 0 || out > max) {
        return fail(error, v, index(i) + index(k),
                    "must be an integer from 0 to " + std::to_string(max));
    }
    return true;
}

bool list_ok(const Value& list, RowError& error) {
    if (list.is_array()) return true;
    return fail(error, list, "", "must be an array");
}

const char kHex[] = "0123456789abcdef";

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

Value note_row(const midi::Note& n) {
    Value v = Value::array();
    v.push(integer(n.start));
    v.push(integer(n.length));
    v.push(integer(n.channel));
    v.push(integer(n.pitch));
    v.push(integer(n.velocity));
    v.push(integer(n.release_velocity));
    return v;
}

Value event_row(const MidiEvent& e) {
    Value v = Value::array();
    v.push(integer(e.tick));
    v.push(integer(e.status));
    v.push(integer(e.data1));
    v.push(integer(e.data2));
    return v;
}

Value sysex_row(const MidiSysEx& x) {
    Value v = Value::array();
    v.push(integer(x.tick));
    std::string hex;
    hex.reserve(x.bytes.size() * 2);
    for (const std::uint8_t b : x.bytes) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0x0F]);
    }
    v.push(Value::string(std::move(hex)));
    return v;
}

bool read_note_rows(const Value& list, std::vector<midi::Note>& out, RowError& error) {
    if (!list_ok(list, error)) return false;
    out.reserve(list.items().size());
    for (std::size_t i = 0; i < list.items().size(); ++i) {
        const Value* r = nullptr;
        if (!row(list, i, 6, "[start, length, channel, pitch, velocity, release_velocity]", error,
                 r)) {
            return false;
        }
        std::int64_t c[6] = {};
        for (std::size_t k = 0; k < 6; ++k) {
            if (!cell(*r, i, k, k < 2 ? kMaxInteger : 255, error, c[k])) return false;
        }
        midi::Note n;
        n.start = c[0];
        n.length = c[1];
        n.channel = static_cast<std::uint8_t>(c[2]);
        n.pitch = static_cast<std::uint8_t>(c[3]);
        n.velocity = static_cast<std::uint8_t>(c[4]);
        n.release_velocity = static_cast<std::uint8_t>(c[5]);
        out.push_back(n);
    }
    return true;
}

bool read_event_rows(const Value& list, std::vector<MidiEvent>& out, RowError& error) {
    if (!list_ok(list, error)) return false;
    out.reserve(list.items().size());
    for (std::size_t i = 0; i < list.items().size(); ++i) {
        const Value* r = nullptr;
        if (!row(list, i, 4, "[tick, status, data1, data2]", error, r)) return false;
        std::int64_t c[4] = {};
        for (std::size_t k = 0; k < 4; ++k) {
            if (!cell(*r, i, k, k == 0 ? kMaxInteger : 255, error, c[k])) return false;
        }
        out.push_back(MidiEvent{c[0], static_cast<std::uint8_t>(c[1]),
                                static_cast<std::uint8_t>(c[2]), static_cast<std::uint8_t>(c[3])});
    }
    return true;
}

bool read_sysex_rows(const Value& list, std::vector<MidiSysEx>& out, RowError& error) {
    if (!list_ok(list, error)) return false;
    out.reserve(list.items().size());
    for (std::size_t i = 0; i < list.items().size(); ++i) {
        const Value* r = nullptr;
        if (!row(list, i, 2, "[tick, \"f0...f7\"]", error, r)) return false;
        MidiSysEx x;
        if (!cell(*r, i, 0, kMaxInteger, error, x.tick)) return false;
        const Value& h = r->items()[1];
        // Checked before decoding, so a hostile file cannot make a large
        // buffer out of one string.
        if (!h.is_string() || h.text().size() % 2 != 0 ||
            h.text().size() > 2 * kMaxSysexBytes) {
            return fail(error, h, index(i) + index(1),
                        "must be the message in lower-case hex, at most " +
                            std::to_string(kMaxSysexBytes) + " bytes");
        }
        x.bytes.reserve(h.text().size() / 2);
        for (std::size_t k = 0; k < h.text().size(); k += 2) {
            const int hi = hex_digit(h.text()[k]);
            const int lo = hex_digit(h.text()[k + 1]);
            if (hi < 0 || lo < 0) {
                return fail(error, h, index(i) + index(1), "must be lower-case hex digits");
            }
            x.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        }
        out.push_back(std::move(x));
    }
    return true;
}

}  // namespace sonora::model::detail
