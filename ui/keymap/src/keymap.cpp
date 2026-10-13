// SPDX-License-Identifier: GPL-3.0-or-later
//
// The JSON underneath is common/json (ADR-0009 §5), shared with the session
// file so the two formats cannot drift into two dialects. Its numbers go
// through std::from_chars, so the C locale no longer decides what "2.5"
// means (BACKLOG: keymap strtod under de_DE).

#include "sonora/keymap.h"

#include "sonora/json/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace sonora {
namespace {

// --- key names -------------------------------------------------------------

struct Alias {
    const char* from;
    const char* to;
};

// Written-in-the-file spelling to canonical name. Punctuation gets a word so
// that "ctrl+[" and "ctrl+bracketleft" are the same binding and neither
// collides with the '+' separator.
const Alias kAliases[] = {
    {"esc", "escape"},
    {"enter", "return"},
    {"ret", "return"},
    {"del", "delete"},
    {"bksp", "backspace"},
    {"pgup", "pageup"},
    {"pgdn", "pagedown"},
    {"page_up", "pageup"},
    {"page_down", "pagedown"},
    {"[", "bracketleft"},
    {"]", "bracketright"},
    {",", "comma"},
    {".", "period"},
    {"/", "slash"},
    {"\\", "backslash"},
    {";", "semicolon"},
    {"'", "apostrophe"},
    {"`", "grave"},
    {"-", "minus"},
    {"=", "equal"},
    {"+", "plus"},
    {"numpad_+", "numpad_plus"},
    {"numpad_-", "numpad_minus"},
    {"numpad_*", "numpad_multiply"},
    {"numpad_/", "numpad_divide"},
    {"numpad_.", "numpad_period"},
    {"kp_0", "numpad_0"},
    {"kp_1", "numpad_1"},
    {"kp_2", "numpad_2"},
    {"kp_3", "numpad_3"},
    {"kp_4", "numpad_4"},
    {"kp_5", "numpad_5"},
    {"kp_6", "numpad_6"},
    {"kp_7", "numpad_7"},
    {"kp_8", "numpad_8"},
    {"kp_9", "numpad_9"},
};

const char* const kNamedKeys[] = {
    "space", "return", "tab", "escape", "backspace", "delete", "insert",
    "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "bracketleft", "bracketright", "comma", "period", "slash", "backslash",
    "semicolon", "apostrophe", "grave", "minus", "equal", "plus",
    "numpad_0", "numpad_1", "numpad_2", "numpad_3", "numpad_4", "numpad_5",
    "numpad_6", "numpad_7", "numpad_8", "numpad_9",
    "numpad_plus", "numpad_minus", "numpad_multiply", "numpad_divide",
    "numpad_period", "numpad_enter",
};

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool known_key(const std::string& key) {
    if (key.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(key[0]);
        if (std::isalnum(c)) return true;
        return false;
    }
    for (const char* k : kNamedKeys) {
        if (key == k) return true;
    }
    return false;
}

std::string canonical_key(const std::string& written) {
    std::string key = lower(written);
    for (const auto& a : kAliases) {
        if (key == a.from) return a.to;
    }
    return key;
}

struct ModName {
    const char* name;
    unsigned bit;
};

const ModName kModNames[] = {
    {"ctrl", kModCtrl},    {"control", kModCtrl}, {"shift", kModShift},
    {"alt", kModAlt},      {"opt", kModAlt},      {"option", kModAlt},
    {"cmd", kModCmd},      {"command", kModCmd},  {"super", kModCmd},
    {"meta", kModCmd},     {"win", kModCmd},
};

std::string at(const std::string& origin, int line) {
    return origin + ":" + std::to_string(line) + ": ";
}

// --- what a terminal actually sends ----------------------------------------
//
// These tables are the single source of truth for both directions:
// TerminalInput decodes with them and KeyMap::terminal_chords() enumerates
// from them. That alone did not keep the two equal -- the enumeration once
// claimed Alt+[ and Alt+Shift+O, which the decoder reads as the start of a
// sequence -- so test_keymap sweeps every one- and two-byte input through the
// decoder and holds the result equal to terminal_chords().

// The characters a US layout prints on the shifted digits, indexed by digit.
// The terminal sends the character, never the key, so "!" is the only
// evidence that Shift+1 was pressed.
const char* const kShiftedDigits = ")!@#$%^&*(";

// Final bytes that name a key with no parameter of their own. 'P'..'S' are the
// SS3 spelling of F1..F4; CSI R is also the cursor-position report, which we
// never ask for, so reading it as F3 is safe here.
struct FinalKey {
    char final;
    const char* key;
};
const FinalKey kFinalKeys[] = {
    {'A', "up"},   {'B', "down"}, {'C', "right"}, {'D', "left"},
    {'H', "home"}, {'F', "end"},  {'P', "f1"},    {'Q', "f2"},
    {'R', "f3"},   {'S', "f4"},
};

// CSI <number> ~ sequences. F1..F4 appear here as 11..14 as well as in the SS3
// table above, because which one a terminal emits depends on the terminal.
struct TildeKey {
    int number;
    const char* key;
};
const TildeKey kTildeKeys[] = {
    {1, "home"},   {2, "insert"}, {3, "delete"}, {4, "end"},
    {5, "pageup"}, {6, "pagedown"},
    {11, "f1"},    {12, "f2"},    {13, "f3"},    {14, "f4"},
    {15, "f5"},    {17, "f6"},    {18, "f7"},    {19, "f8"},
    {20, "f9"},    {21, "f10"},   {23, "f11"},   {24, "f12"},
};

// Every modifier set xterm's parameter can carry. Cmd is absent because the
// protocol has no way to say it.
const unsigned kModCombos[] = {
    kModNone,
    kModShift,
    kModAlt,
    kModCtrl,
    kModShift | kModAlt,
    kModShift | kModCtrl,
    kModAlt | kModCtrl,
    kModShift | kModAlt | kModCtrl,
};

// xterm sends one plus a bit set: 1 shift, 2 alt, 4 ctrl.
unsigned mods_from_param(int param) {
    const int bits = param - 1;
    unsigned m = kModNone;
    if (bits & 1) m |= kModShift;
    if (bits & 2) m |= kModAlt;
    if (bits & 4) m |= kModCtrl;
    return m;
}

// The Linux console's F1..F5: ESC [ [ then A..E. xterm and the console agree
// on everything from F6 up, which is in kTildeKeys.
const FinalKey kLinuxConsoleKeys[] = {
    {'A', "f1"}, {'B', "f2"}, {'C', "f3"}, {'D', "f4"}, {'E', "f5"},
};

enum class Scan {
    Incomplete,  // the sequence may still be arriving
    Complete,    // a whole sequence ends at `end`
    Broken,      // a byte that cannot belong to it sits at `end`
};

bool is_final_byte(unsigned char c) { return c >= 0x40 && c <= 0x7e; }

// Finds the end of the ESC-introduced sequence that starts at buf[at].
// ECMA-48 shape: parameter bytes 0x30-0x3f, then intermediates 0x20-0x2f, then
// one final byte 0x40-0x7e. Anything else -- a control byte, ESC above all --
// cannot be part of the sequence: it is the next key press, so the sequence
// is broken just before it and that byte is left to be decoded on its own.
// Taking it as a final byte is how ESC O followed by an up arrow lost the
// arrow's ESC and dispatched '[' and shift+a.
Scan scan_sequence(const std::string& buf, std::size_t at, std::size_t& end) {
    std::size_t i = at + 2;
    const auto finish = [&](std::size_t last) {
        if (is_final_byte(static_cast<unsigned char>(buf[last]))) {
            end = last + 1;
            return Scan::Complete;
        }
        end = last;
        return Scan::Broken;
    };
    if (buf[at + 1] == 'O') {
        // Old xterm puts a modifier between ESC O and the final byte: Ctrl+Up
        // is ESC O 5 A. Taking the 5 as the final byte left "5" and "A" to be
        // dispatched as key presses, and the default map arms track 5 on "5".
        while (i < buf.size()) {
            const unsigned char c = static_cast<unsigned char>(buf[i]);
            if (c >= 0x30 && c <= 0x3f) {
                ++i;
                continue;
            }
            return finish(i);
        }
        return Scan::Incomplete;
    }
    // '[' is itself a final byte, so without this the console's F1 ended at
    // ESC [ [ and its letter was dispatched as a key of its own.
    if (i < buf.size() && buf[i] == '[') {
        ++i;
        if (i >= buf.size()) return Scan::Incomplete;
        return finish(i);
    }
    while (i < buf.size()) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c >= 0x20 && c <= 0x3f) {
            ++i;  // parameter or intermediate
            continue;
        }
        return finish(i);
    }
    return Scan::Incomplete;
}

bool decode_sequence(const std::string& seq, KeyChord& out) {
    const char final_byte = seq[seq.size() - 1];
    if (seq.size() == 4 && seq[1] == '[' && seq[2] == '[') {
        for (const auto& f : kLinuxConsoleKeys) {
            if (f.final == final_byte) {
                out.key = f.key;
                out.mods = kModNone;
                return true;
            }
        }
        return false;
    }
    // Between the introducer and the final byte.
    const std::string params = seq.substr(2, seq.size() - 3);

    std::vector<int> fields;
    if (!params.empty()) {
        int n = 0;
        bool any = false;
        for (const char c : params) {
            if (c == ';') {
                fields.push_back(any ? n : 0);
                n = 0;
                any = false;
            } else if (c >= '0' && c <= '9') {
                n = n * 10 + (c - '0');
                any = true;
                if (n > 100000) return false;
            } else {
                return false;  // a private-use sequence we have no name for
            }
        }
        fields.push_back(any ? n : 0);
    }
    if (seq[1] == 'O') {
        // SS3 carries at most the modifier, alone (ESC O 5 A) or after a 1
        // (ESC O 1 ; 5 A), and only on the keys that have an SS3 form.
        if (fields.size() > 2 || final_byte == '~') return false;
        if (!fields.empty() && (fields.back() < 1 || fields.back() > 8)) {
            return false;
        }
        if (final_byte == 'Z') {
            if (!fields.empty()) return false;
            out.key = "tab";
            out.mods = kModShift;
            return true;
        }
        for (const auto& f : kFinalKeys) {
            if (f.final == final_byte) {
                out.key = f.key;
                out.mods = fields.empty() ? kModNone : mods_from_param(fields.back());
                return true;
            }
        }
        return false;
    }
    const unsigned mods =
        fields.size() > 1 ? mods_from_param(fields[1]) : kModNone;

    if (final_byte == '~') {
        if (fields.empty()) return false;
        for (const auto& t : kTildeKeys) {
            if (t.number == fields[0]) {
                out.key = t.key;
                out.mods = mods;
                return true;
            }
        }
        return false;
    }
    if (final_byte == 'Z') {
        // xterm's Shift+Tab, which carries no parameter.
        out.key = "tab";
        out.mods = kModShift;
        return true;
    }
    for (const auto& f : kFinalKeys) {
        if (f.final == final_byte) {
            out.key = f.key;
            out.mods = mods;
            return true;
        }
    }
    return false;
}

bool contains_chord(const std::vector<KeyChord>& set, const KeyChord& c) {
    for (const auto& e : set) {
        if (e == c) return true;
    }
    return false;
}

std::string join(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += sep;
        out += items[i];
    }
    return out;
}

}  // namespace

bool needs_selection(const Binding& b) {
    const CommandSpec* spec = find_command(b.command);
    return spec != nullptr && spec->target == CommandTarget::Track &&
           !b.ctx.has_target();
}

namespace {

bool runs_member(const Binding& b, const std::vector<std::string>& group,
                 const std::vector<KeyChord>& device,
                 const std::function<bool(const std::string&)>& implemented) {
    if (needs_selection(b) || !implemented(b.command) ||
        !contains_chord(device, b.chord)) {
        return false;
    }
    for (const auto& id : group) {
        if (b.command == id) return true;
    }
    return false;
}

}  // namespace

bool KeyMap::reaches(
    const std::vector<std::string>& group, const std::vector<KeyChord>& device,
    const std::function<bool(const std::string&)>& implemented) const {
    for (const Binding& b : bindings_) {
        if (runs_member(b, group, device, implemented)) return true;
    }
    return false;
}

bool KeyMap::parse_chord(const std::string& spec, KeyChord& out,
                         std::string& error) {
    if (spec.empty()) {
        error = "an empty key";
        return false;
    }

    KeyChord chord;
    std::string rest = spec;

    // Peel modifiers off the front. Scanning forwards rather than splitting on
    // '+' is what lets "ctrl++" and "numpad_+" both work.
    for (;;) {
        bool matched = false;
        for (const auto& m : kModNames) {
            const std::size_t n = std::strlen(m.name);
            if (rest.size() > n + 1 && lower(rest.substr(0, n)) == m.name &&
                rest[n] == '+') {
                if (chord.mods & m.bit) {
                    error = "the modifier \"" + std::string(m.name) +
                            "\" appears twice";
                    return false;
                }
                chord.mods |= m.bit;
                rest = rest.substr(n + 1);
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }

    chord.key = canonical_key(rest);
    if (chord.key.empty()) {
        error = "no key after the modifiers";
        return false;
    }
    if (!known_key(chord.key)) {
        error = "\"" + rest + "\" is not a key name this build knows";
        return false;
    }
    // An upper-case letter in a spec is a spelling of shift, not a key.
    if (rest.size() == 1 && std::isupper(static_cast<unsigned char>(rest[0]))) {
        chord.mods |= kModShift;
    }

    out = chord;
    return true;
}

std::string KeyMap::chord_to_string(const KeyChord& chord) {
    std::string s;
    if (chord.mods & kModCtrl) s += "ctrl+";
    if (chord.mods & kModAlt) s += "alt+";
    if (chord.mods & kModShift) s += "shift+";
    if (chord.mods & kModCmd) s += "cmd+";
    s += chord.key;
    return s;
}

bool KeyMap::chord_from_terminal_byte(unsigned char byte, KeyChord& out) {
    out = KeyChord{};
    switch (byte) {
        case 0x00:
            // NUL is what Ctrl+Space sends. A terminal sends it for Ctrl+@ and
            // Ctrl+2 as well and nothing distinguishes the three, so this names
            // the one an operator means and the one a profile actually binds.
            out.key = "space";
            out.mods = kModCtrl;
            return true;
        case ' ': out.key = "space"; return true;
        case '\t': out.key = "tab"; return true;
        case '\r':
        case '\n': out.key = "return"; return true;
        case 0x1b: out.key = "escape"; return true;
        case 0x08:
        case 0x7f: out.key = "backspace"; return true;
        default: break;
    }
    if (byte >= 1 && byte <= 26) {
        out.key = std::string(1, static_cast<char>('a' + byte - 1));
        out.mods = kModCtrl;
        return true;
    }
    if (byte >= 0x20 && byte < 0x7f) {
        const char c = static_cast<char>(byte);
        if (std::isupper(static_cast<unsigned char>(c))) {
            out.key = std::string(1, static_cast<char>(std::tolower(c)));
            out.mods = kModShift;
            return true;
        }
        // The shifted digits. Without this every "shift+<digit>" binding is
        // unreachable at a tty, because "!" is not alnum and so is not a key
        // name this build knows -- which is what stopped protools.json and
        // logic.json from arming a track at all.
        for (int d = 0; d < 10; ++d) {
            if (c == kShiftedDigits[d]) {
                out.key = std::string(1, static_cast<char>('0' + d));
                out.mods = kModShift;
                return true;
            }
        }
        const std::string key = canonical_key(std::string(1, c));
        if (!known_key(key)) return false;
        out.key = key;
        return true;
    }
    return false;
}

std::vector<KeyChord> KeyMap::terminal_chords() {
    return terminal_chords([](unsigned char) { return true; });
}

std::vector<KeyChord> KeyMap::terminal_chords(
    const std::function<bool(unsigned char)>& delivered) {
    std::vector<KeyChord> chords;
    const auto add = [&chords](const KeyChord& c) {
        if (!contains_chord(chords, c)) chords.push_back(c);
    };
    const bool escape_arrives = delivered(0x1b);

    for (int b = 0; b < 256; ++b) {
        const unsigned char byte = static_cast<unsigned char>(b);
        if (!delivered(byte)) continue;
        KeyChord c;
        if (!chord_from_terminal_byte(byte, c)) continue;
        add(c);
        // ESC then the same byte, which is the convention for Alt+it -- except
        // where ESC and that byte begin a sequence instead.
        if (!escape_arrives || byte == '[' || byte == 'O') continue;
        KeyChord with_alt = c;
        with_alt.mods |= kModAlt;
        add(with_alt);
    }
    if (!escape_arrives) return chords;

    for (const auto& f : kFinalKeys) {
        for (const unsigned m : kModCombos) add(KeyChord{f.key, m});
    }
    for (const auto& t : kTildeKeys) {
        for (const unsigned m : kModCombos) add(KeyChord{t.key, m});
    }
    for (const auto& f : kLinuxConsoleKeys) {
        add(KeyChord{f.key, kModNone});
        add(KeyChord{f.key, kModAlt});
    }
    add(KeyChord{"tab", kModShift});
    add(KeyChord{"tab", kModShift | kModAlt});
    return chords;
}

void TerminalInput::feed(const char* bytes, std::size_t count) {
    buf_.append(bytes, count);
}

TerminalInput::Result TerminalInput::next(bool idle, KeyChord& out) {
    out = KeyChord{};
    if (buf_.empty()) return Result::None;

    const unsigned char b0 = static_cast<unsigned char>(buf_[0]);
    if (b0 != 0x1b) {
        buf_.erase(0, 1);
        return KeyMap::chord_from_terminal_byte(b0, out) ? Result::Chord
                                                         : Result::Unrecognised;
    }
    if (buf_.size() == 1) {
        // Indistinguishable from the start of a sequence until the timeout
        // says nothing more is coming.
        if (!idle) return Result::NeedMore;
        buf_.erase(0, 1);
        out.key = "escape";
        return Result::Chord;
    }

    // rxvt sends Alt+<key> as ESC in front of the key's own sequence, so ESC
    // ESC [ A is Alt+Up. Reading the first two bytes as Alt+Escape left "[A"
    // to be dispatched as two more key presses.
    std::size_t at = 0;
    unsigned alt = kModNone;
    if (buf_[1] == '\x1b') {
        if (buf_.size() == 2 && !idle) return Result::NeedMore;
        if (buf_.size() > 2 && (buf_[2] == '[' || buf_[2] == 'O')) {
            at = 1;
            alt = kModAlt;
        }
    }

    if (buf_[at + 1] == '[' || buf_[at + 1] == 'O') {
        std::size_t end = 0;
        switch (scan_sequence(buf_, at, end)) {
            case Scan::Incomplete:
                if (!idle) return Result::NeedMore;
                // Truncated and nothing more coming: drop the lot. Replaying
                // the tail as key presses is how an up arrow used to execute
                // a command.
                buf_.clear();
                return Result::Unrecognised;
            case Scan::Broken:
                buf_.erase(0, end);
                return Result::Unrecognised;
            case Scan::Complete:
                break;
        }
        const std::string seq = buf_.substr(at, end - at);
        buf_.erase(0, end);
        if (!decode_sequence(seq, out)) return Result::Unrecognised;
        out.mods |= alt;
        return Result::Chord;
    }
    const unsigned char b1 = static_cast<unsigned char>(buf_[1]);
    buf_.erase(0, 2);
    if (!KeyMap::chord_from_terminal_byte(b1, out)) return Result::Unrecognised;
    out.mods |= kModAlt;
    return Result::Chord;
}

std::vector<TerminalInput::Event> TerminalInput::frame(const char* bytes,
                                                       long count) {
    if (count > 0) feed(bytes, static_cast<std::size_t>(count));
    // A frame in which nothing arrived is the escape timeout: the gap between
    // bytes inside one sequence is far shorter than a frame, the gap between
    // two human key presses far longer.
    const bool idle = count <= 0;
    std::vector<Event> events;
    for (;;) {
        Event e;
        e.result = next(idle, e.chord);
        if (e.result == Result::None || e.result == Result::NeedMore) break;
        events.push_back(std::move(e));
    }
    return events;
}

const Binding* KeyMap::resolve(const KeyChord& chord) const {
    for (const auto& b : bindings_) {
        if (b.chord == chord) return &b;
    }
    return nullptr;
}

const Binding* KeyMap::resolve(const std::string& spec) const {
    KeyChord chord;
    std::string err;
    if (!parse_chord(spec, chord, err)) return nullptr;
    return resolve(chord);
}

bool KeyMap::load_file(const std::string& path, KeyMap& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot be opened for reading";
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return load_string(buf.str(), path, out, error);
}

bool KeyMap::load_string(const std::string& text, const std::string& origin,
                         KeyMap& out, std::string& error) {
    json::Value root;
    json::Error parse_error;
    if (!json::parse(text, root, parse_error)) {
        error = at(origin, parse_error.line) + parse_error.message;
        return false;
    }
    if (!root.is_object()) {
        error = at(origin, root.line()) + "the top level must be an object";
        return false;
    }

    KeyMap map;
    const json::Value* bindings = nullptr;

    for (const auto& kv : root.members()) {
        if (kv.first == "name") {
            if (!kv.second.is_string()) {
                error = at(origin, kv.second.line()) + "\"name\" must be a string";
                return false;
            }
            map.name_ = kv.second.text();
        } else if (kv.first == "comment") {
            if (!kv.second.is_string()) {
                error = at(origin, kv.second.line()) + "\"comment\" must be a string";
                return false;
            }
            map.comment_ = kv.second.text();
        } else if (kv.first == "bindings") {
            if (!kv.second.is_object()) {
                error = at(origin, kv.second.line()) +
                        "\"bindings\" must be an object of key to command";
                return false;
            }
            bindings = &kv.second;
        } else {
            error = at(origin, kv.second.line()) + "unknown top-level member \"" +
                    kv.first + "\" (expected name, comment or bindings)";
            return false;
        }
    }

    if (map.name_.empty()) {
        error = at(origin, root.line()) + "no \"name\"";
        return false;
    }
    if (bindings == nullptr) {
        error = at(origin, root.line()) + "no \"bindings\" object";
        return false;
    }

    for (const auto& kv : bindings->members()) {
        const std::string& spec = kv.first;
        const json::Value& v = kv.second;
        const int line = v.line();

        Binding b;
        b.spec = spec;
        b.line = line;

        std::string why;
        if (!parse_chord(spec, b.chord, why)) {
            error = at(origin, line) + "key \"" + spec + "\": " + why;
            return false;
        }

        bool has_target = false;
        bool has_argument = false;
        bool has_verified = false;
        long target = 0;

        if (v.is_string()) {
            b.command = v.text();
        } else if (v.is_object()) {
            for (const auto& f : v.members()) {
                if (f.first == "command") {
                    if (!f.second.is_string()) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"command\" must be a string";
                        return false;
                    }
                    b.command = f.second.text();
                } else if (f.first == "target") {
                    if (!f.second.is_number()) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"target\" must be a track number";
                        return false;
                    }
                    // Range-checked as a double before any narrowing. Casting
                    // first is undefined for a value a long cannot hold, and
                    // in practice 4294967297 landed on track 1 -- a plausible
                    // file, no error, the wrong track.
                    // The parser already refused a number past a double's
                    // range, so this conversion cannot fail.
                    double n = 0.0;
                    f.second.to_double(n);
                    if (!(n >= 1.0 &&
                          n <= static_cast<double>(kMaxTrackTarget)) ||
                        n != std::floor(n)) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"target\" must be a whole track number"
                                " from 1 to " +
                                std::to_string(kMaxTrackTarget);
                        return false;
                    }
                    target = static_cast<long>(n);
                    has_target = true;
                } else if (f.first == "argument") {
                    if (!f.second.is_string()) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"argument\" must be a string";
                        return false;
                    }
                    // Checked against the command below, for the same reason
                    // "target" is: a value the command will never read is a
                    // mistake in the file, not a harmless extra.
                    has_argument = true;
                    b.ctx.argument = f.second.text();
                } else if (f.first == "verified") {
                    if (!f.second.is_bool()) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"verified\" must be true or false";
                        return false;
                    }
                    b.verified = f.second.as_bool();
                    has_verified = true;
                } else if (f.first == "origin") {
                    if (!f.second.is_string() ||
                        (f.second.text() != "vendor" && f.second.text() != "sonora")) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"origin\" must be \"vendor\" or"
                                " \"sonora\"";
                        return false;
                    }
                    b.origin = f.second.text() == "sonora" ? BindingOrigin::Sonora
                                                        : BindingOrigin::Vendor;
                } else if (f.first == "comment") {
                    if (!f.second.is_string()) {
                        error = at(origin, f.second.line()) + "key \"" + spec +
                                "\": \"comment\" must be a string";
                        return false;
                    }
                    b.comment = f.second.text();
                } else {
                    error = at(origin, f.second.line()) + "key \"" + spec +
                            "\": unknown member \"" + f.first + "\"";
                    return false;
                }
            }
            if (b.command.empty()) {
                error = at(origin, line) + "key \"" + spec +
                        "\": no \"command\"";
                return false;
            }
            // "verified" is about the vendor's documentation, which a chord
            // of ours is not in. Accepting both would bring back the
            // ambiguity "origin" exists to remove.
            if (has_verified && b.origin == BindingOrigin::Sonora) {
                error = at(origin, line) + "key \"" + spec +
                        "\": \"origin\": \"sonora\" has no vendor binding"
                        " to be \"verified\" against";
                return false;
            }
        } else {
            error = at(origin, line) + "key \"" + spec +
                    "\": the value must be a command name or an object";
            return false;
        }

        const CommandSpec* cmd = find_command(b.command);
        if (cmd == nullptr) {
            // The loud failure keymaps/README.md asks for. A key bound to a
            // command the engine has never heard of would do nothing, and an
            // operator discovers that mid-take.
            error = at(origin, line) + "key \"" + spec +
                    "\": unknown command \"" + b.command + "\"";
            return false;
        }
        if (has_argument && !cmd->takes_argument) {
            error = at(origin, line) + "key \"" + spec + "\": command \"" +
                    b.command + "\" does not take an argument";
            return false;
        }
        if (has_target) {
            if (cmd->target != CommandTarget::Track) {
                error = at(origin, line) + "key \"" + spec + "\": command \"" +
                        b.command + "\" does not take a target";
                return false;
            }
            // Written 1-based because that is what the operator sees on the
            // track; stored 0-based because that is how tracks() is indexed.
            b.ctx.target = static_cast<int>(target - 1);
        }

        for (const auto& existing : map.bindings_) {
            if (existing.chord == b.chord) {
                error = at(origin, line) + "key \"" + spec +
                        "\" is the same chord as \"" + existing.spec +
                        "\" on line " + std::to_string(existing.line);
                return false;
            }
        }

        map.bindings_.push_back(std::move(b));
    }

    out = std::move(map);
    return true;
}

const std::vector<std::vector<std::string>>& KeyMap::essential_commands() {
    // What an operator has to be able to do at a tty. Any other command
    // missing from a profile is an inconvenience; one of these missing means
    // the appliance is not a recorder, or is one you cannot leave. Satisfying
    // any one member of a group satisfies the group.
    //
    // Starting and stopping a take are separate groups because a profile can
    // put them on different keys -- Pro Tools starts on Command+Space and
    // stops on Space -- and one that can only start has no way to close the
    // file short of quitting. transport.toggle_play is in the stop group:
    // its handler closes a running take (session_commands.cpp), and where a
    // build cannot run it, it does not count.
    static const std::vector<std::vector<std::string>> groups = {
        {"track.toggle_arm", "track.arm", "track.arm_all"},
        {"transport.toggle_record", "transport.record"},
        {"transport.toggle_record", "transport.stop", "transport.toggle_play"},
        {"app.quit"},
    };
    return groups;
}

// Why this warns and repairs instead of refusing the profile outright.
//
// Both options break a rule. Loading a profile that cannot arm a track is a
// keyboard that lies, which the charter puts above a missing one. But refusing
// to start is worse here than it is anywhere else in this engine: the engine
// *is* the appliance -- systemd restarts it, there is no shell to fall back
// to, and an operator holding a USB stick and a band in the room would get a
// restart loop and no recorder. Refusing is the right answer when proceeding
// would produce a bad take silently (that is why --record with no connected
// input exits non-zero); it is the wrong answer when a loud, specific message
// plus a working keyboard is available instead.
//
// So: name every essential the profile cannot reach, name the key we bound in
// its place and what that key used to do, and keep recording possible. The
// operator is told exactly what is not as their profile says -- which is the
// part that matters -- and still has an engine. The messages are printed to
// stderr at start-up *and* held on screen by the interface, because a notice
// the next redraw erases is a notice nobody read.
//
// A repair must never cost the operator an essential the profile already had:
// rebinding q to app.quit in a profile whose only arm key was q traded one
// dead essential for another and reported success. So a substitute goes on a
// chord the profile leaves free if the default offers one; failing that it
// may take over one chord, preferring a binding that cannot work in this
// build anyway, and only where every essential that worked before still
// works after. If neither is possible the notice says the keyboard cannot do
// it, which is the truth.
std::vector<std::string> KeyMap::ensure_essentials(
    const std::vector<KeyChord>& device,
    const std::function<bool(const std::string&)>& implemented) {
    using Group = std::vector<std::string>;
    std::vector<std::string> report;
    const KeyMap fallback = built_in_default();
    const auto& groups = essential_commands();
    const std::vector<Binding> as_given = bindings_;
    const auto group_with = [&groups](const char* id) -> const Group* {
        for (const auto& g : groups) {
            for (const auto& member : g) {
                if (member == id) return &g;
            }
        }
        return nullptr;
    };
    const Group* const start_group = group_with("transport.record");
    const Group* const stop_group = group_with("transport.stop");

    const auto usable = [&](const Binding& b, const Group& group) {
        return runs_member(b, group, device, implemented);
    };
    const auto satisfied = [&](const std::vector<Binding>& bindings,
                               const Group& group) {
        for (const auto& b : bindings) {
            if (usable(b, group)) return true;
        }
        return false;
    };
    const auto keeps_what_works = [&](const std::vector<Binding>& after) {
        for (const auto& g : groups) {
            if (satisfied(bindings_, g) && !satisfied(after, g)) return false;
        }
        return true;
    };
    const auto describe = [](const Binding& b) {
        std::string note = "\"" + b.spec + "\" -> " + b.command;
        if (b.ctx.has_target()) {
            note += " (track " + std::to_string(b.ctx.target + 1) + ")";
        }
        return note;
    };

    std::vector<const Group*> reported_unfixable;
    for (const auto& group : groups) {
        if (satisfied(bindings_, group)) continue;
        const std::string names = join(group, " or ");

        std::vector<std::string> took;

        // A profile that starts a take on a key of its own and stops it on
        // its play/stop key -- Pro Tools' Ctrl+Space and Space, Logic's R and
        // Space -- has a dead play/stop key in a build without playback.
        // The default's substitute, toggle_record, would make that key start
        // a take where the operator expects playback; transport.stop on it
        // stops a take as the application's Space does and never starts one.
        // Only a play/stop key that cannot work in this build is taken.
        if (&group == stop_group && start_group != nullptr &&
            satisfied(as_given, *start_group)) {
            for (std::size_t at = 0; at < bindings_.size(); ++at) {
                const Binding& occupant = bindings_[at];
                if (occupant.command != "transport.toggle_play" ||
                    implemented(occupant.command)) {
                    continue;
                }
                std::vector<Binding> after = bindings_;
                after[at].command = "transport.stop";
                after[at].ctx = CommandContext{};
                after[at].comment =
                    "substituted for a missing essential; see keymaps/README.md";
                if (!usable(after[at], group) || !keeps_what_works(after)) continue;
                took.push_back(describe(after[at]) + " (was " + occupant.command +
                               ")");
                bindings_ = std::move(after);
                break;
            }
            if (!took.empty()) {
                report.push_back("no key this terminal can send runs " + names +
                                 "; this profile's own play/stop key now stops"
                                 " a take, and never starts one: " +
                                 join(took, ", "));
                continue;
            }
        }

        bool default_has_one = false;
        for (const Binding& src : fallback.bindings_) {
            if (!usable(src, group)) continue;
            default_has_one = true;
            if (resolve(src.chord) != nullptr) continue;
            Binding b = src;
            b.comment = "substituted for a missing essential; see keymaps/README.md";
            took.push_back(describe(b));
            bindings_.push_back(std::move(b));
        }

        for (const bool dead_only : {true, false}) {
            if (!took.empty()) break;
            for (const Binding& src : fallback.bindings_) {
                if (!usable(src, group)) continue;
                std::size_t at = 0;
                while (at < bindings_.size() && bindings_[at].chord != src.chord) ++at;
                if (at == bindings_.size()) continue;
                const Binding& occupant = bindings_[at];
                const bool dead = !implemented(occupant.command) ||
                                  needs_selection(occupant);
                if (dead_only && !dead) continue;

                std::vector<Binding> after = bindings_;
                after[at] = src;
                after[at].comment =
                    "substituted for a missing essential; see keymaps/README.md";
                if (!keeps_what_works(after)) continue;
                took.push_back(describe(src) + " (was " + occupant.command + ")");
                bindings_ = std::move(after);
                break;
            }
        }

        if (took.empty()) {
            reported_unfixable.push_back(&group);
            report.push_back(
                "no key this terminal can send runs " + names +
                (default_has_one
                     ? ", and every key the built-in default uses for it is"
                       " needed for another essential"
                     : ", and the built-in default has no substitute either") +
                ": this keyboard cannot do it at all");
            continue;
        }
        report.push_back("no key this terminal can send runs " + names +
                         "; bound from the built-in default instead: " +
                         join(took, ", "));
    }

    // The rule above should make this unreachable. If it ever is not, the
    // operator hears it here instead of trusting the notices above.
    for (const auto& group : groups) {
        if (satisfied(bindings_, group)) continue;
        bool said = false;
        for (const Group* g : reported_unfixable) said = said || g == &group;
        if (said) continue;
        report.push_back("after repair, no key this terminal can send runs " +
                         join(group, " or ") +
                         ": this keyboard cannot do it at all");
    }
    return report;
}

KeyMap KeyMap::built_in_default() {
    // Kept identical to keymaps/default.json, which test_keymap enforces.
    static const char* const kDefault = R"JSON({
  "name": "Default",
  "comment": "Matches what the engine and the terminal interface implement today. Phase 1 only.",
  "bindings": {
    "space": "transport.toggle_record",
    "1": { "command": "track.toggle_arm", "target": 1 },
    "2": { "command": "track.toggle_arm", "target": 2 },
    "3": { "command": "track.toggle_arm", "target": 3 },
    "4": { "command": "track.toggle_arm", "target": 4 },
    "5": { "command": "track.toggle_arm", "target": 5 },
    "6": { "command": "track.toggle_arm", "target": 6 },
    "7": { "command": "track.toggle_arm", "target": 7 },
    "8": { "command": "track.toggle_arm", "target": 8 },
    "a": "track.arm_all",
    "q": "app.quit"
  }
})JSON";

    KeyMap map;
    std::string error;
    if (!load_string(kDefault, "<built-in default>", map, error)) {
        // Only reachable by editing the literal above into something invalid,
        // which is a programming error in our own source and not a condition
        // the operator can do anything about. Returning the empty map instead
        // would ship exactly the dead keyboard this function exists to
        // prevent, and the interface's next redraw would erase the warning.
        // test_keymap calls this, so the abort lands in CI, not in a studio.
        std::fprintf(stderr, "sonora: the built-in key map is broken: %s\n",
                     error.c_str());
        std::abort();
    }
    return map;
}

}  // namespace sonora
