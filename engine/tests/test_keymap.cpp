// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/keymap.h"

#include <termios.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "sonora/command.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"
#include "sonora/tty_mode.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

#ifndef SONORA_KEYMAP_DIR
#define SONORA_KEYMAP_DIR "keymaps"
#endif

namespace {

using sonora::Binding;
using sonora::CommandContext;
using sonora::CommandRegistry;
using sonora::dispatch_result_text;
using sonora::DispatchResult;
using sonora::KeyChord;
using sonora::KeyMap;
using sonora::TerminalInput;

bool loads(const std::string& json, KeyMap& out, std::string& error) {
    return KeyMap::load_string(json, "<test>", out, error);
}

// A message is only useful if it names the thing that is wrong.
bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool sendable(const std::vector<KeyChord>& device, const KeyChord& c) {
    for (const auto& e : device) {
        if (e == c) return true;
    }
    return false;
}

// Feeds a whole byte sequence and takes the one chord it should produce.
// `idle` is true at the end of the feed, which is how the real loop behaves
// once the 50 ms frame passes with nothing more waiting.
TerminalInput::Result decode(const char* bytes, std::size_t n, KeyChord& out) {
    TerminalInput in;
    in.feed(bytes, n);
    TerminalInput::Result r = in.next(false, out);
    if (r == TerminalInput::Result::NeedMore) r = in.next(true, out);
    return r;
}

TerminalInput::Result decode(const char* literal, KeyChord& out) {
    return decode(literal, std::strlen(literal), out);
}

// The handlers sonora-engine really installs, on a Session that never opens
// an audio device. A list of them kept here instead went stale the first
// time a handler was added, and the essentials test then passed about a build
// nobody ships.
class BuildCommands {
public:
    BuildCommands() : session_(config()) {
        sonora::install_session_commands(reg_, session_, take_, quit_);
    }
    bool operator()(const std::string& id) const { return reg_.implemented(id); }

private:
    static sonora::SessionConfig config() {
        sonora::SessionConfig cfg;
        cfg.dir = "test_keymap_unused";
        cfg.track_count = 1;
        cfg.ring_seconds = 0.01;
        return cfg;
    }

    sonora::Session session_;
    CommandRegistry reg_;
    int take_ = 1;
    bool quit_ = false;
};

bool anything_is_implemented(const std::string&) { return true; }

using Implemented = std::function<bool(const std::string&)>;

bool satisfied(const KeyMap& map, const std::vector<std::string>& group,
               const std::vector<KeyChord>& device,
               const Implemented& implemented) {
    for (const Binding& b : map.bindings()) {
        for (const std::string& id : group) {
            if (b.command == id && implemented(b.command) &&
                sendable(device, b.chord) && !sonora::needs_selection(b)) {
                return true;
            }
        }
    }
    return false;
}

const std::vector<std::string>& group_containing(const char* id) {
    for (const auto& g : KeyMap::essential_commands()) {
        for (const auto& member : g) {
            if (member == id) return g;
        }
    }
    static const std::vector<std::string> none;
    return none;
}

void add_unique(std::vector<KeyChord>& set, const KeyChord& c) {
    if (!sendable(set, c)) set.push_back(c);
}

// Everything the decoder produces from `bytes` arriving in one read, followed
// by a frame in which nothing arrives -- the shape of the real loop.
void collect(const std::string& bytes, std::vector<KeyChord>& out) {
    TerminalInput in;
    in.feed(bytes.data(), bytes.size());
    for (const bool idle : {false, true}) {
        for (int guard = 0; guard < 64; ++guard) {
            KeyChord c;
            const TerminalInput::Result r = in.next(idle, c);
            if (r == TerminalInput::Result::None ||
                r == TerminalInput::Result::NeedMore) {
                break;
            }
            if (r == TerminalInput::Result::Chord) add_unique(out, c);
        }
    }
}

// Written out by hand from the xterm and Linux console documentation rather
// than generated from keymap.cpp's tables, so that a table entry the decoder
// gains or loses shows up as a difference instead of agreeing with itself.
std::vector<std::string> known_sequences() {
    std::vector<std::string> seqs;
    const std::string csi = "\033[";
    for (const char f : std::string("ABCDHFPQRSZ")) {
        seqs.push_back(csi + f);
        seqs.push_back(std::string("\033O") + f);
        for (int m = 2; m <= 8; ++m) {
            seqs.push_back(csi + "1;" + std::to_string(m) + f);
        }
    }
    for (const int n : {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 17, 18, 19, 20,
                        21, 23, 24}) {
        seqs.push_back(csi + std::to_string(n) + "~");
        for (int m = 2; m <= 8; ++m) {
            seqs.push_back(csi + std::to_string(n) + ";" + std::to_string(m) +
                           "~");
        }
    }
    // The Linux console's F1..F5.
    for (char f = 'A'; f <= 'E'; ++f) seqs.push_back(csi + "[" + f);
    // rxvt's Alt+key: ESC in front of the key's own sequence.
    const std::size_t plain = seqs.size();
    for (std::size_t i = 0; i < plain; ++i) seqs.push_back("\033" + seqs[i]);
    return seqs;
}

std::vector<TerminalInput::Result> results_of(const char* bytes, std::size_t n,
                                              std::vector<KeyChord>& chords) {
    TerminalInput in;
    in.feed(bytes, n);
    std::vector<TerminalInput::Result> rs;
    for (const bool idle : {false, true}) {
        for (int guard = 0; guard < 64; ++guard) {
            KeyChord c;
            const TerminalInput::Result r = in.next(idle, c);
            if (r == TerminalInput::Result::None ||
                r == TerminalInput::Result::NeedMore) {
                break;
            }
            rs.push_back(r);
            if (r == TerminalInput::Result::Chord) chords.push_back(c);
        }
    }
    return rs;
}

// --- the terminal mode delivers every byte the decoder names ---
//
// Only ICANON and ECHO used to be cleared. On tty1 that left IXON on, so
// Ctrl+S froze the console (draw() then blocked in printf mid-take) and
// Ctrl+Q -- Pro Tools' quit -- was eaten; and ISIG on, so Ctrl+C killed the
// engine and Ctrl+Z stopped it with a take open. Every one of those chords
// was in terminal_chords(), so ensure_essentials() believed them.
termios cooked_console() {
    termios cooked{};
    cooked.c_iflag = ICRNL | IXON | IXOFF | BRKINT;
    cooked.c_oflag = OPOST | ONLCR;
    cooked.c_lflag = ICANON | ECHO | ECHOE | ISIG | IEXTEN;
    cooked.c_cc[VINTR] = 0x03;
    cooked.c_cc[VQUIT] = 0x1c;
    cooked.c_cc[VERASE] = 0x7f;
    cooked.c_cc[VKILL] = 0x15;
    cooked.c_cc[VEOF] = 0x04;
    cooked.c_cc[VSTART] = 0x11;
    cooked.c_cc[VSTOP] = 0x13;
    cooked.c_cc[VSUSP] = 0x1a;
    cooked.c_cc[VREPRINT] = 0x12;
    cooked.c_cc[VWERASE] = 0x17;
    cooked.c_cc[VLNEXT] = 0x16;
    cooked.c_cc[VDISCARD] = 0x0f;
    cooked.c_cc[VMIN] = 1;
    cooked.c_cc[VTIME] = 0;
    return cooked;
}

int test_key_reading_mode() {
    const termios cooked = cooked_console();

    // The model has to be able to see the defect, or passing proves nothing:
    // the old mode, ICANON and ECHO cleared and nothing else.
    termios old_mode = cooked;
    old_mode.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    CHECK(!sonora::tty_delivers_byte(old_mode, 0x13));  // Ctrl+S
    CHECK(!sonora::tty_delivers_byte(old_mode, 0x11));  // Ctrl+Q
    CHECK(!sonora::tty_delivers_byte(old_mode, 0x03));  // Ctrl+C
    CHECK(!sonora::tty_delivers_byte(old_mode, 0x1a));  // Ctrl+Z
    CHECK(sonora::tty_delivers_byte(old_mode, 'q'));
    CHECK(!sonora::tty_delivers_byte(cooked, 'q'));     // canonical: no keys

    const termios mode = sonora::key_reading_mode(cooked);
    CHECK((mode.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)) == 0);
    CHECK((mode.c_iflag & (IXON | IXOFF)) == 0);
    // Output processing stays on: draw() writes "\n" and relies on the
    // terminal turning it into a new line at column zero.
    CHECK((mode.c_oflag & OPOST) != 0);
    CHECK(mode.c_cc[VMIN] == 0);
    CHECK(mode.c_cc[VTIME] == 0);

    int failures = 0;
    for (int b = 0; b < 256; ++b) {
        KeyChord c;
        if (!KeyMap::chord_from_terminal_byte(static_cast<unsigned char>(b), c)) {
            continue;
        }
        if (!sonora::tty_delivers_byte(mode, static_cast<unsigned char>(b))) {
            std::fprintf(stderr,
                         "FAIL byte 0x%02x (%s) never reaches the engine in"
                         " key-reading mode\n",
                         b, KeyMap::chord_to_string(c).c_str());
            ++failures;
        }
    }
    CHECK(sonora::tty_delivers_byte(mode, 0x1b));

    // The device ensure_essentials() measures against follows the mode. Under
    // the old one these were claimed sendable, which is how a Pro Tools
    // profile passed the quit check with a quit key flow control ate.
    const auto device_in = [](const termios& t) {
        return KeyMap::terminal_chords(
            [&t](unsigned char b) { return sonora::tty_delivers_byte(t, b); });
    };
    const std::vector<KeyChord> old_device = device_in(old_mode);
    for (const char* spec : {"ctrl+q", "ctrl+s", "ctrl+c", "ctrl+z", "alt+ctrl+q"}) {
        KeyChord c;
        std::string why;
        CHECK(KeyMap::parse_chord(spec, c, why));
        CHECK(!sendable(old_device, c));
    }
    const std::vector<KeyChord> new_device = device_in(mode);
    const std::vector<KeyChord> everything = KeyMap::terminal_chords();
    CHECK(new_device.size() == everything.size());
    for (const KeyChord& c : everything) CHECK(sendable(new_device, c));
    return failures == 0 ? 0 : 1;
}

// --- a profile that cannot quit keeps Ctrl+C ---
//
// The key-reading mode turns ISIG off so Ctrl+C can be edit.copy. When no key
// the profile has can run app.quit -- and ensure_essentials() could not repair
// that -- the operator had no keyboard way out at all. main.cpp then switches
// to this mode, in which Ctrl+C raises SIGINT and the engine stops cleanly,
// and measures the profile against what is left.
int test_interrupt_key_when_nothing_quits(const Implemented& implemented) {
    termios cooked = cooked_console();
    // Whatever the console had, the notice promises Ctrl+C.
    cooked.c_cc[VINTR] = static_cast<cc_t>(_POSIX_VDISABLE);
    const termios mode = sonora::interrupt_key_mode(cooked);
    CHECK((mode.c_lflag & ISIG) != 0);
    CHECK((mode.c_lflag & (ICANON | ECHO | IEXTEN)) == 0);
    CHECK((mode.c_iflag & (IXON | IXOFF)) == 0);
    CHECK(mode.c_cc[VINTR] == 0x03);
    CHECK(!sonora::tty_delivers_byte(mode, 0x03));
    // Only the interrupt: Ctrl+Z stopping the engine with a take open, or
    // Ctrl+\ dumping core, is no way out.
    CHECK(sonora::tty_delivers_byte(mode, 0x1a));
    CHECK(sonora::tty_delivers_byte(mode, 0x1c));
    CHECK(sonora::tty_delivers_byte(mode, 'q'));

    const std::vector<KeyChord> device = KeyMap::terminal_chords(
        [&mode](unsigned char b) { return sonora::tty_delivers_byte(mode, b); });
    const std::vector<KeyChord> everything = KeyMap::terminal_chords();
    CHECK(device.size() + 2 == everything.size());
    for (const char* gone : {"ctrl+c", "alt+ctrl+c"}) {
        KeyChord c;
        std::string why;
        CHECK(KeyMap::parse_chord(gone, c, why));
        CHECK(!sendable(device, c));
    }

    const std::vector<std::string>& quit = group_containing("app.quit");
    CHECK(!quit.empty());
    {
        KeyMap m;
        std::string err;
        // q is the only arm key, so the default's quit cannot have it.
        CHECK(loads(R"({ "name": "x", "bindings": {
            "q": "track.arm_all",
            "space": "transport.toggle_record" } })", m, err));
        m.ensure_essentials(everything, implemented);
        CHECK(!m.reaches(quit, everything, implemented));

        // Measured again under the new mode, what worked still works.
        KeyMap again;
        CHECK(loads(R"({ "name": "x", "bindings": {
            "ctrl+z": { "command": "track.toggle_arm", "target": 1 },
            "space": "transport.toggle_record" } })", again, err));
        again.ensure_essentials(device, implemented);
        CHECK(again.resolve("ctrl+z")->command == "track.toggle_arm");
        CHECK(again.reaches(group_containing("track.toggle_arm"), device,
                            implemented));
    }
    {
        KeyMap d = KeyMap::built_in_default();
        CHECK(d.reaches(quit, everything, implemented));
    }
    return 0;
}

// --- the escape timeout ---
//
// A lone ESC and the first byte of an arrow key are the same byte; only time
// tells them apart. The interface loop's frame is that clock: a frame in which
// read() returns nothing ends whatever is pending.
int test_escape_timeout() {
    using R = TerminalInput::Result;
    {
        TerminalInput in;
        CHECK(in.frame("\033", 1).empty());
        const auto e = in.frame(nullptr, 0);
        CHECK(e.size() == 1);
        CHECK(e[0].result == R::Chord);
        CHECK(e[0].chord.key == "escape" && e[0].chord.mods == sonora::kModNone);
        CHECK(!in.pending());
    }
    {
        // A sequence split across reads waits for its tail.
        TerminalInput in;
        CHECK(in.frame("\033[1;5", 5).empty());
        const auto e = in.frame("C", 1);
        CHECK(e.size() == 1);
        CHECK(e[0].chord.key == "right" && e[0].chord.mods == sonora::kModCtrl);
    }
    {
        // read() failing -- EAGAIN, or EINTR when SIGTERM lands -- is a frame
        // in which nothing arrived, and a truncated sequence is dropped whole.
        TerminalInput in;
        CHECK(in.frame("\033[", 2).empty());
        const auto e = in.frame(nullptr, -1);
        CHECK(e.size() == 1);
        CHECK(e[0].result == R::Unrecognised);
        CHECK(!in.pending());
    }
    {
        // Keys ahead of a trailing ESC come out at once; only the ESC waits.
        TerminalInput in;
        auto e = in.frame("q\033", 2);
        CHECK(e.size() == 1);
        CHECK(e[0].chord.key == "q");
        CHECK(in.pending());
        e = in.frame(nullptr, 0);
        CHECK(e.size() == 1);
        CHECK(e[0].chord.key == "escape");
    }
    {
        // Alt+x arrives as ESC x in one read, and is never Escape then x.
        TerminalInput in;
        const auto e = in.frame("\033x", 2);
        CHECK(e.size() == 1);
        CHECK(e[0].chord.key == "x" && e[0].chord.mods == sonora::kModAlt);
    }
    return 0;
}

// --- the Linux console's F1..F5 ---
//
// tty1 sends F1 as ESC [ [ A. The second '[' ended the sequence, so ESC [ [
// was dropped as unrecognised and the A that followed armed nothing and
// dispatched shift+a.
int test_linux_console_function_keys() {
    const char* const names[] = {"f1", "f2", "f3", "f4", "f5"};
    for (int i = 0; i < 5; ++i) {
        const std::string seq = std::string("\033[[") + static_cast<char>('A' + i);
        std::vector<KeyChord> chords;
        const auto rs = results_of(seq.data(), seq.size(), chords);
        CHECK(rs.size() == 1);
        CHECK(rs[0] == TerminalInput::Result::Chord);
        CHECK(chords.size() == 1);
        CHECK(chords[0].key == names[i]);
        CHECK(chords[0].mods == sonora::kModNone);
    }
    // Arriving a byte at a time is still one key.
    TerminalInput in;
    KeyChord c;
    in.feed("\033[", 2);
    CHECK(in.next(false, c) == TerminalInput::Result::NeedMore);
    in.feed("[", 1);
    CHECK(in.next(false, c) == TerminalInput::Result::NeedMore);
    in.feed("C", 1);
    CHECK(in.next(false, c) == TerminalInput::Result::Chord);
    CHECK(c.key == "f3");
    CHECK(in.next(true, c) == TerminalInput::Result::None);
    return 0;
}

// --- an ESC never ends somebody else's sequence ---
//
// ESC O ESC was taken as a complete SS3 sequence, so a stray ESC O followed
// by an up arrow in the same read lost the arrow's ESC and dispatched '[' and
// shift+a. The same happened to a broken CSI.
int test_escape_starts_a_new_sequence() {
    {
        std::vector<KeyChord> chords;
        const auto rs = results_of("\033O\033[A", 5, chords);
        CHECK(rs.size() == 2);
        CHECK(rs[0] == TerminalInput::Result::Unrecognised);
        CHECK(chords.size() == 1);
        CHECK(chords[0].key == "up" && chords[0].mods == sonora::kModNone);
    }
    {
        std::vector<KeyChord> chords;
        const auto rs = results_of("\033[1;\033[B", 7, chords);
        CHECK(rs.size() == 2);
        CHECK(rs[0] == TerminalInput::Result::Unrecognised);
        CHECK(chords.size() == 1);
        CHECK(chords[0].key == "down");
    }
    {
        // rxvt's Alt+Up. Read as Alt+Escape it left "[A" to be dispatched as
        // two more key presses.
        std::vector<KeyChord> chords;
        const auto rs = results_of("\033\033[A", 4, chords);
        CHECK(rs.size() == 1);
        CHECK(chords.size() == 1);
        CHECK(chords[0].key == "up" && chords[0].mods == sonora::kModAlt);
    }
    {
        // A real Alt+Escape still is one, once nothing more arrives.
        std::vector<KeyChord> chords;
        const auto rs = results_of("\033\033", 2, chords);
        CHECK(rs.size() == 1);
        CHECK(chords.size() == 1);
        CHECK(chords[0].key == "escape" && chords[0].mods == sonora::kModAlt);
    }
    return 0;
}

// --- SS3 with a modifier parameter is one key ---
//
// Old xterm sends Ctrl+Up as ESC O 5 A. The SS3 scan took the byte after ESC O
// as the final byte, so this decoded as an unrecognised ESC O 5, a stray "5"
// and shift+a -- and under the default map the "5" toggled arm on track 5.
int test_ss3_modifier_parameter() {
    using R = TerminalInput::Result;
    struct Case {
        const char* bytes;
        const char* key;
        unsigned mods;
    };
    const Case cases[] = {
        {"\033O5A", "up", sonora::kModCtrl},
        {"\033O2P", "f1", sonora::kModShift},
        {"\033O3D", "left", sonora::kModAlt},
        {"\033O1;5C", "right", sonora::kModCtrl},
    };
    for (const Case& c : cases) {
        std::vector<KeyChord> chords;
        const auto rs = results_of(c.bytes, std::strlen(c.bytes), chords);
        CHECK(rs.size() == 1);
        CHECK(rs[0] == R::Chord);
        CHECK(chords.size() == 1);
        CHECK(chords[0].key == c.key);
        CHECK(chords[0].mods == c.mods);
    }
    {
        // Split across reads, it is still one key, and nothing in between.
        TerminalInput in;
        CHECK(in.frame("\033O5", 3).empty());
        const auto e = in.frame("A", 1);
        CHECK(e.size() == 1);
        CHECK(e[0].chord.key == "up" && e[0].chord.mods == sonora::kModCtrl);
    }
    {
        // Parameters with no final byte, or a final we have no name for, are
        // one unrecognised sequence -- never a digit press.
        for (const char* bytes : {"\033O5", "\033O5~", "\033O5;5;5A"}) {
            std::vector<KeyChord> chords;
            const auto rs = results_of(bytes, std::strlen(bytes), chords);
            CHECK(rs.size() == 1);
            CHECK(rs[0] == R::Unrecognised);
            CHECK(chords.empty());
        }
    }
    {
        // What the default map does with it: nothing gets armed.
        const KeyMap map = KeyMap::built_in_default();
        TerminalInput in;
        for (const auto& e : in.frame("\033O5A", 4)) {
            if (e.result != R::Chord) continue;
            const Binding* b = map.resolve(e.chord);
            CHECK(b == nullptr || b->command != "track.toggle_arm");
        }
    }
    return 0;
}

// --- the decoder and terminal_chords() describe the same set ---
//
// terminal_chords() is what ensure_essentials() trusts. It listed
// alt+bracketleft and alt+shift+o, which the decoder can never produce
// because ESC [ and ESC O introduce sequences; a profile whose only quit key
// was one of those passed the check and could not quit.
int test_decoder_matches_terminal_chords() {
    std::vector<KeyChord> decoded;
    for (int a = 0; a < 256; ++a) {
        collect(std::string(1, static_cast<char>(a)), decoded);
        for (int b = 0; b < 256; ++b) {
            std::string two;
            two += static_cast<char>(a);
            two += static_cast<char>(b);
            collect(two, decoded);
        }
    }
    for (const std::string& s : known_sequences()) collect(s, decoded);

    const std::vector<KeyChord> claimed = KeyMap::terminal_chords();
    int failures = 0;
    for (const KeyChord& c : decoded) {
        if (!sendable(claimed, c)) {
            std::fprintf(stderr, "FAIL the decoder produces %s, which"
                         " terminal_chords() does not list\n",
                         KeyMap::chord_to_string(c).c_str());
            ++failures;
        }
    }
    for (const KeyChord& c : claimed) {
        if (!sendable(decoded, c)) {
            std::fprintf(stderr, "FAIL terminal_chords() lists %s, which no"
                         " input decodes to\n",
                         KeyMap::chord_to_string(c).c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

// --- the JSON reader is as strict as JSON ---
int test_json_strictness() {
    KeyMap map;
    std::string err;

    // RFC 8259: a leading zero is not a number. "01" quietly reading as 1 is
    // how a file that is not JSON gets past the one tool that said it was.
    CHECK(!loads(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": 01 } } })", map, err));
    CHECK(mentions(err, "leading zero"));
    err.clear();
    CHECK(!loads(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": -01 } } })", map, err));
    CHECK(mentions(err, "leading zero"));
    // Zero itself, and a fraction of it, are still numbers.
    err.clear();
    CHECK(!loads(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": 0 } } })", map, err));
    CHECK(mentions(err, "from 1 to"));
    err.clear();
    CHECK(!loads(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": 0.5 } } })", map, err));
    CHECK(mentions(err, "from 1 to"));

    return 0;
}

int test_json_control_characters() {
    KeyMap map;
    std::string err;
    // A raw control character inside a string is not JSON; it is usually an
    // editor's tab that the operator cannot see.
    CHECK(!loads("{ \"name\": \"a\tb\", \"bindings\": {} }", map, err));
    CHECK(mentions(err, "control character"));
    err.clear();
    CHECK(!loads(std::string("{ \"name\": \"a\x01" "b\", \"bindings\": {} }"),
                 map, err));
    CHECK(mentions(err, "control character"));
    // The escaped form is fine.
    err.clear();
    CHECK(loads("{ \"name\": \"a\\tb\", \"bindings\": {} }", map, err));

    return 0;
}

int test_json_duplicate_line() {
    KeyMap map;
    std::string err;
    // A duplicate is reported on the line of the key that repeats, not the
    // line where its value happens to end.
    CHECK(!loads("{\n"
                 "  \"name\": \"x\",\n"
                 "  \"bindings\": {\n"
                 "    \"q\": \"app.quit\",\n"
                 "    \"q\": {\n"
                 "      \"command\": \"app.quit\"\n"
                 "    }\n"
                 "  }\n"
                 "}", map, err));
    CHECK(mentions(err, "duplicate member"));
    CHECK(mentions(err, "<test>:5:"));
    return 0;
}

// --- a repair never breaks an essential that was already satisfied ---
//
// "q": track.arm_all, no app.quit: the quit repair rebound q, the profile lost
// its only arm key, and ensure_essentials() reported success.
int test_repair_keeps_what_works(const std::vector<KeyChord>& device,
                                 const Implemented& implemented) {
    {
        KeyMap m;
        std::string err;
        CHECK(loads(R"({ "name": "x", "bindings": {
            "q": "track.arm_all",
            "space": "transport.toggle_record" } })", m, err));
        const std::vector<std::string> notices =
            m.ensure_essentials(device, implemented);
        CHECK(m.resolve("q") != nullptr);
        CHECK(m.resolve("q")->command == "track.arm_all");
        CHECK(satisfied(m, group_containing("track.arm_all"), device,
                        implemented));
        // And it says so, rather than claiming a repair it did not make.
        bool says_cannot_quit = false;
        for (const std::string& n : notices) {
            if (mentions(n, "app.quit") && mentions(n, "cannot")) {
                says_cannot_quit = true;
            }
        }
        CHECK(says_cannot_quit);
    }
    {
        // A free chord is used before one that is in use.
        KeyMap m;
        std::string err;
        CHECK(loads(R"({ "name": "x", "bindings": {
            "a": "app.quit",
            "space": "transport.toggle_record" } })", m, err));
        CHECK(!m.ensure_essentials(device, implemented).empty());
        CHECK(m.resolve("a")->command == "app.quit");
        CHECK(m.resolve("1") != nullptr);
        CHECK(m.resolve("1")->command == "track.toggle_arm");
        for (const auto& g : KeyMap::essential_commands()) {
            CHECK(satisfied(m, g, device, implemented));
        }
    }
    {
        // A profile that starts a take on its own key and stops on its
        // play/stop key -- Pro Tools' ctrl+space and space -- gets
        // transport.stop on that key, not the default's toggle_record. Space
        // pressed while stopped means "play" there; with toggle_record it
        // started a junk take. The notice names what the key used to do, and
        // does not claim the substitute came from the built-in default.
        KeyMap m;
        std::string err;
        CHECK(loads(R"({ "name": "x", "bindings": {
            "space": "transport.toggle_play",
            "ctrl+space": "transport.record",
            "1": { "command": "track.toggle_arm", "target": 1 },
            "q": "app.quit" } })", m, err));
        const std::vector<std::string> notices =
            m.ensure_essentials(device, implemented);
        CHECK(notices.size() == 1);
        CHECK(mentions(notices[0], "\"space\" -> transport.stop"
                                   " (was transport.toggle_play)"));
        CHECK(!mentions(notices[0], "built-in default"));
        CHECK(m.resolve("space")->command == "transport.stop");
        for (const auto& g : KeyMap::essential_commands()) {
            CHECK(satisfied(m, g, device, implemented));
        }
    }
    {
        // With no start key of its own, the default's toggle_record is what
        // starts a take at all, and it goes on the play/stop key as before.
        KeyMap m;
        std::string err;
        CHECK(loads(R"({ "name": "x", "bindings": {
            "space": "transport.toggle_play",
            "1": { "command": "track.toggle_arm", "target": 1 },
            "q": "app.quit" } })", m, err));
        const std::vector<std::string> notices =
            m.ensure_essentials(device, implemented);
        CHECK(notices.size() == 1);
        CHECK(mentions(notices[0], "built-in default"));
        CHECK(m.resolve("space")->command == "transport.toggle_record");
        for (const auto& g : KeyMap::essential_commands()) {
            CHECK(satisfied(m, g, device, implemented));
        }
    }
    return 0;
}

// --- a target-less arm is not a way to arm ---
//
// track.toggle_arm with no target fails in this build, because there is no
// selection for it to act on; the essentials check counted it anyway.
int test_targetless_arm_does_not_count(const std::vector<KeyChord>& device,
                                       const Implemented& implemented) {
    KeyMap m;
    std::string err;
    CHECK(loads(R"({ "name": "x", "bindings": {
        "shift+r": "track.toggle_arm",
        "space": "transport.toggle_record",
        "q": "app.quit" } })", m, err));
    const std::vector<std::string> notices =
        m.ensure_essentials(device, implemented);
    CHECK(notices.size() == 1);
    CHECK(mentions(notices[0], "track.toggle_arm"));
    CHECK(m.resolve("1") != nullptr);
    CHECK(m.resolve("1")->ctx.target == 0);
    // The profile's own binding is left alone; it is not wrong, only early.
    CHECK(m.resolve("shift+r")->command == "track.toggle_arm");
    return 0;
}

// --- the handlers the engine installs ---
int test_session_commands() {
    sonora::SessionConfig cfg;
    cfg.dir = "test_keymap_session";
    cfg.track_count = 4;
    cfg.ring_seconds = 0.25;
    const std::string wav = cfg.dir + "/Audio Files/Audio 1_01.wav";
    int result = 0;
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit);

        // No selection exists, so there is nothing for it to act on, and
        // guessing track 1 would be the port-ordering defect again.
        CHECK(reg.dispatch("track.toggle_arm") == DispatchResult::Failed);

        CommandContext first;
        first.target = 0;
        CHECK(reg.dispatch("track.toggle_arm", first) == DispatchResult::Ok);
        CHECK(s.tracks()[0]->rec_armed());

        // Pro Tools' Command+Space and F12, Logic's R: start, never stop.
        s.disk().start();
        CHECK(reg.implemented("transport.record"));
        CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        CHECK(take == 1);
        CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(take == 2);

        // Nothing armed means nothing to record, and it says so.
        CHECK(reg.dispatch("track.disarm_all") == DispatchResult::Ok);
        CHECK(reg.dispatch("transport.record") == DispatchResult::Failed);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);

        CHECK(reg.dispatch("app.quit") == DispatchResult::Ok);
        CHECK(quit);
    }
    if (std::remove(wav.c_str()) != 0) result = 1;
    std::remove((cfg.dir + "/Audio Files").c_str());
    std::remove(cfg.dir.c_str());
    if (result != 0) std::fprintf(stderr, "FAIL %s was not written\n", wav.c_str());
    return result;
}

// --- arming is fixed for the length of a take ---
//
// Session::process() writes a track only while it is armed, but its file stays
// open: disarming mid-take and re-arming resumed the file with no gap, so
// that track came out shorter than the rest and out of sync from that point.
// Arming a track with no file open filled a ring nobody drained and counted
// overruns. So every arming command refuses during a take, and says why.
int test_arming_locked_during_take() {
    sonora::SessionConfig cfg;
    cfg.dir = "test_keymap_arming";
    cfg.track_count = 3;
    cfg.ring_seconds = 0.25;
    std::string recorded;
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit);

        CommandContext first;
        first.target = 0;
        CommandContext second;
        second.target = 1;
        CHECK(reg.dispatch("track.toggle_arm", first) == DispatchResult::Ok);
        s.disk().start();
        CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        recorded = s.tracks()[0]->writer().path();

        struct Attempt {
            const char* command;
            CommandContext ctx;
        };
        const Attempt attempts[] = {
            {"track.toggle_arm", first},   // would leave a gap in track 1
            {"track.toggle_arm", second},  // would fill a ring with no file
            {"track.arm_all", {}},
            {"track.disarm_all", {}},
        };
        for (const Attempt& a : attempts) {
            std::string why;
            CHECK(reg.dispatch(a.command, a.ctx, why) == DispatchResult::Failed);
            CHECK(mentions(why, "during a take"));
            CHECK(s.tracks()[0]->rec_armed());
            CHECK(!s.tracks()[1]->rec_armed());
            CHECK(!s.tracks()[2]->rec_armed());
        }

        // Once the take is closed, arming works again.
        CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
        CHECK(reg.dispatch("track.toggle_arm", second) == DispatchResult::Ok);
        CHECK(s.tracks()[1]->rec_armed());
        CHECK(reg.dispatch("track.disarm_all") == DispatchResult::Ok);
        CHECK(!s.tracks()[0]->rec_armed());
    }
    std::remove(recorded.c_str());
    std::remove((cfg.dir + "/Audio Files").c_str());
    std::remove(cfg.dir.c_str());
    return 0;
}

// --- the corrections in brain/research/protools-logic-key-bindings.md §8 ---
int test_profile_corrections() {
    struct Expect {
        const char* file;
        const char* chord;
        const char* command;  // nullptr: must be unbound
    };
    const Expect expects[] = {
        {"protools.json", "ctrl+7", "tool.smart"},
        {"protools.json", "ctrl+f7", nullptr},
        {"protools.json", "alt+f", "zoom.fit_selection"},
        {"protools.json", "alt+a", "zoom.fit_session"},
        {"protools.json", "ctrl+space", "transport.record"},
        {"protools.json", "alt+shift+d", nullptr},  // Duplicate Tracks
        {"protools.json", "shift+r", "track.toggle_arm"},
        {"protools.json", "shift+s", "track.toggle_solo"},
        {"protools.json", "shift+m", "track.toggle_mute"},
        {"protools.json", "shift+i", "track.toggle_input_monitor"},
        {"logic.json", "cmd+[", "edit.trim_start_to_insertion"},
        {"logic.json", "cmd+]", "edit.trim_end_to_insertion"},
        {"logic.json", "ctrl+alt+[", nullptr},
        {"logic.json", "ctrl+alt+]", nullptr},
        {"logic.json", "ctrl+alt+x", "edit.fade_selection"},
        {"logic.json", "cmd+f", nullptr},        // Show/Hide Flex Pitch/Time
        {"logic.json", "numpad_multiply", "transport.toggle_record"},
        {"logic.json", "cmd+d", nullptr},        // New Track with Duplicate Settings
        {"logic.json", "alt+pagedown", "tool.next"},
        {"logic.json", "t", nullptr},            // Show Tool Menu
        {"logic.json", "alt+z", nullptr},        // Navigation: Back
        {"logic.json", "ctrl+alt+1", nullptr},   // Set Nudge Value to 1 ms
        {"logic.json", "ctrl+grave", nullptr},   // Scroll in Play
        {"logic.json", "alt+shift+r", nullptr},  // Region Inspector Float
        {"logic.json", "ctrl+r", "track.toggle_arm"},
        {"logic.json", "ctrl+i", "track.toggle_input_monitor"},
        {"logic.json", "up", "track.select_previous"},
        {"logic.json", "down", "track.select_next"},
    };
    for (const Expect& e : expects) {
        const std::string path = std::string(SONORA_KEYMAP_DIR) + "/" + e.file;
        KeyMap map;
        std::string err;
        CHECK(KeyMap::load_file(path, map, err));
        const Binding* b = map.resolve(e.chord);
        const bool ok = e.command == nullptr
                            ? b == nullptr
                            : (b != nullptr && b->command == e.command &&
                               !b->ctx.has_target());
        if (!ok) {
            std::fprintf(stderr, "FAIL %s: %s is %s, the research note says %s\n",
                         e.file, e.chord, b ? b->command.c_str() : "unbound",
                         e.command ? e.command : "unbound");
            return 1;
        }
    }

    KeyMap logic;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/logic.json",
                            logic, err));
    CHECK(!mentions(logic.comment(), "by number"));
    for (const Binding& b : logic.bindings()) {
        if (mentions(b.comment, "no default key")) {
            std::fprintf(stderr, "FAIL logic.json: %s still says Logic has no"
                         " default key\n", b.spec.c_str());
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main() {
    const BuildCommands build_commands;
    const Implemented implemented = [&build_commands](const std::string& id) {
        return build_commands(id);
    };

    // Run to completion and count, so one failure does not hide the next.
    {
        const std::vector<KeyChord> device = KeyMap::terminal_chords();
        int failures = 0;
        failures += test_key_reading_mode();
        failures += test_interrupt_key_when_nothing_quits(implemented);
        failures += test_linux_console_function_keys();
        failures += test_escape_starts_a_new_sequence();
        failures += test_escape_timeout();
        failures += test_ss3_modifier_parameter();
        failures += test_decoder_matches_terminal_chords();
        failures += test_json_strictness();
        failures += test_json_control_characters();
        failures += test_json_duplicate_line();
        failures += test_repair_keeps_what_works(device, implemented);
        failures += test_targetless_arm_does_not_count(device,
                                                       implemented);
        failures += test_session_commands();
        failures += test_arming_locked_during_take();
        failures += test_profile_corrections();
        if (failures != 0) {
            std::fprintf(stderr, "FAIL %d regression tests\n", failures);
            return 1;
        }
    }

    // --- a valid map loads ---
    {
        KeyMap map;
        std::string err;
        CHECK(loads(R"({
            "name": "Tiny",
            "bindings": {
                "space": "transport.toggle_record",
                "ctrl+shift+s": "app.save",
                "3": { "command": "track.toggle_arm", "target": 3 }
            }
        })", map, err));
        CHECK(err.empty());
        CHECK(map.name() == "Tiny");
        CHECK(map.bindings().size() == 3);

        const Binding* b = map.resolve("space");
        CHECK(b != nullptr);
        CHECK(b->command == "transport.toggle_record");
        CHECK(b->ctx.has_target() == false);
        CHECK(b->verified);

        // A target is written as the operator sees it and stored as an index.
        const Binding* t = map.resolve("3");
        CHECK(t != nullptr);
        CHECK(t->command == "track.toggle_arm");
        CHECK(t->ctx.target == 2);
    }

    // --- a map naming an unknown command fails, loudly and precisely ---
    {
        KeyMap map;
        std::string err;
        CHECK(!loads("{\n"
                     "  \"name\": \"Broken\",\n"
                     "  \"bindings\": {\n"
                     "    \"space\": \"transport.toggle_record\",\n"
                     "    \"w\": \"transport.wrap_it_up\"\n"
                     "  }\n"
                     "}", map, err));
        CHECK(mentions(err, "unknown command"));
        CHECK(mentions(err, "transport.wrap_it_up"));
        CHECK(mentions(err, "\"w\""));
        CHECK(mentions(err, "<test>:5"));  // the line the bad binding is on
        // Nothing partially loaded: a half-applied key map is the failure
        // mode this test exists to prevent.
        CHECK(map.bindings().empty());
    }

    // --- malformed JSON says where and what ---
    {
        KeyMap map;
        std::string err;
        CHECK(!loads("{\n  \"name\": \"Oops\",\n  \"bindings\": {\n"
                     "    \"space\": \"transport.play\",\n  }\n}",
                     map, err));
        CHECK(mentions(err, "<test>:5"));
        CHECK(!err.empty());

        err.clear();
        CHECK(!loads("{ \"name\": \"Oops\", \"bindings\": }", map, err));
        CHECK(mentions(err, "<test>:1"));

        err.clear();
        CHECK(!loads("not json at all", map, err));
        CHECK(!err.empty());

        err.clear();
        CHECK(!loads("{ \"name\": \"Oops\", \"bindings\": { \"space\": \"a",
                     map, err));
        CHECK(mentions(err, "unterminated string"));
    }

    // --- schema violations are refused rather than ignored ---
    {
        KeyMap map;
        std::string err;

        CHECK(!loads(R"({ "name": "x", "bindings": {}, "extra": 1 })", map, err));
        CHECK(mentions(err, "unknown top-level member"));

        err.clear();
        CHECK(!loads(R"({ "bindings": {} })", map, err));
        CHECK(mentions(err, "name"));

        err.clear();
        CHECK(!loads(R"({ "name": "x" })", map, err));
        CHECK(mentions(err, "bindings"));

        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "app.quit", "targte": 1 } } })", map, err));
        CHECK(mentions(err, "unknown member"));

        // A target on a command that ignores targets is a mistake, not a
        // harmless extra field.
        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "app.quit", "target": 1 } } })", map, err));
        CHECK(mentions(err, "does not take a target"));

        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.arm", "target": 0 } } })", map, err));
        CHECK(mentions(err, "from 1 to"));

        // An argument on a command that never reads one is the same class of
        // mistake as a target on a command that ignores targets, and was the
        // one the schema accepted silently.
        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "app.quit", "argument": "zzz" } } })", map, err));
        CHECK(mentions(err, "does not take an argument"));
        CHECK(mentions(err, "app.quit"));

        // An unknown key name is as bad as an unknown command.
        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": { "f27": "app.quit" } })",
                     map, err));
        CHECK(mentions(err, "f27"));

        // Two spellings of the same chord cannot both win.
        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "ctrl+[": "zoom.out",
            "ctrl+bracketleft": "zoom.in" } })", map, err));
        CHECK(mentions(err, "same chord"));
    }

    // --- modifiers parse, in any order, with any spelling ---
    {
        KeyChord c;
        std::string err;
        CHECK(KeyMap::parse_chord("ctrl+shift+s", c, err));
        CHECK(c.key == "s");
        CHECK(c.mods == (sonora::kModCtrl | sonora::kModShift));

        KeyChord d;
        CHECK(KeyMap::parse_chord("shift+ctrl+s", d, err));
        CHECK(c == d);

        KeyChord e;
        CHECK(KeyMap::parse_chord("control+Shift+s", e, err));
        CHECK(c == e);

        // Mac spellings are aliases, not separate modifiers.
        KeyChord opt;
        CHECK(KeyMap::parse_chord("option+a", opt, err));
        KeyChord alt;
        CHECK(KeyMap::parse_chord("alt+a", alt, err));
        CHECK(opt == alt);

        // An upper-case letter is a spelling of shift.
        KeyChord upper;
        CHECK(KeyMap::parse_chord("S", upper, err));
        CHECK(upper.key == "s");
        CHECK(upper.mods == sonora::kModShift);

        // Punctuation survives the '+' separator.
        KeyChord plus;
        CHECK(KeyMap::parse_chord("ctrl++", plus, err));
        CHECK(plus.key == "plus");
        CHECK(plus.mods == sonora::kModCtrl);

        KeyChord kp;
        CHECK(KeyMap::parse_chord("numpad_+", kp, err));
        CHECK(kp.key == "numpad_plus");
        CHECK(kp.mods == sonora::kModNone);

        CHECK(KeyMap::chord_to_string(c) == "ctrl+shift+s");

        // Failures are reported, not guessed at.
        CHECK(!KeyMap::parse_chord("", c, err));
        CHECK(!KeyMap::parse_chord("ctrl+", c, err));
        CHECK(!KeyMap::parse_chord("ctrl+ctrl+a", c, err));
        CHECK(!KeyMap::parse_chord("hyper+a", c, err));
    }

    // --- the same key with different modifiers is a different command ---
    {
        KeyMap map;
        std::string err;
        CHECK(loads(R"({
            "name": "Mods",
            "bindings": {
                "s": "track.toggle_solo",
                "ctrl+s": "app.save",
                "shift+s": "edit.trim_end_to_insertion",
                "ctrl+shift+s": "app.quit"
            }
        })", map, err));
        CHECK(map.resolve("s")->command == "track.toggle_solo");
        CHECK(map.resolve("ctrl+s")->command == "app.save");
        CHECK(map.resolve("shift+s")->command == "edit.trim_end_to_insertion");
        CHECK(map.resolve("S")->command == "edit.trim_end_to_insertion");
        CHECK(map.resolve("ctrl+shift+s")->command == "app.quit");
        CHECK(map.resolve("alt+s") == nullptr);
    }

    // --- a raw terminal byte becomes a chord ---
    {
        KeyChord c;
        CHECK(KeyMap::chord_from_terminal_byte(' ', c));
        CHECK(c.key == "space" && c.mods == sonora::kModNone);

        CHECK(KeyMap::chord_from_terminal_byte('q', c));
        CHECK(c.key == "q");

        CHECK(KeyMap::chord_from_terminal_byte('A', c));
        CHECK(c.key == "a" && c.mods == sonora::kModShift);

        CHECK(KeyMap::chord_from_terminal_byte(0x03, c));  // Ctrl+C
        CHECK(c.key == "c" && c.mods == sonora::kModCtrl);

        CHECK(KeyMap::chord_from_terminal_byte('\r', c));
        CHECK(c.key == "return");

        CHECK(KeyMap::chord_from_terminal_byte('[', c));
        CHECK(c.key == "bracketleft");

        // The shifted digits arrive as the character the layout prints, and
        // reading them back is the only way "shift+1" is reachable at a tty.
        CHECK(KeyMap::chord_from_terminal_byte('!', c));
        CHECK(c.key == "1" && c.mods == sonora::kModShift);
        CHECK(KeyMap::chord_from_terminal_byte('*', c));
        CHECK(c.key == "8" && c.mods == sonora::kModShift);
        CHECK(KeyMap::chord_from_terminal_byte(')', c));
        CHECK(c.key == "0" && c.mods == sonora::kModShift);

        // NUL is Ctrl+Space, which is how protools.json reaches
        // transport.toggle_record.
        CHECK(KeyMap::chord_from_terminal_byte(0, c));
        CHECK(c.key == "space" && c.mods == sonora::kModCtrl);

        CHECK(!KeyMap::chord_from_terminal_byte(0xff, c));
    }

    // --- byte sequences become chords, and nothing else ---
    //
    // The bug this replaces: one byte per read mapped 0x1b to Escape with no
    // lookahead, so Up arrow dispatched whatever Escape was bound to and then
    // two more key presses, and F5 under the default map armed tracks 1 and 5.
    {
        KeyChord c;
        CHECK(decode("\033[A", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "up" && c.mods == sonora::kModNone);
        CHECK(decode("\033[B", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "down");
        CHECK(decode("\033[C", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "right");
        CHECK(decode("\033[D", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "left");

        CHECK(decode("\033[15~", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "f5" && c.mods == sonora::kModNone);
        CHECK(decode("\033[24~", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "f12");
        CHECK(decode("\033OP", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "f1");
        CHECK(decode("\033[3~", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "delete");
        CHECK(decode("\033[Z", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "tab" && c.mods == sonora::kModShift);

        // xterm's modifier parameter: 1 plus shift|alt<<1|ctrl<<2.
        CHECK(decode("\033[1;5C", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "right" && c.mods == sonora::kModCtrl);
        CHECK(decode("\033[1;4A", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "up" && c.mods == (sonora::kModShift | sonora::kModAlt));

        // ESC then a byte is Alt+that byte, which is how every "alt+..."
        // binding in the shipped profiles is reached at all.
        CHECK(decode("\033r", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "r" && c.mods == sonora::kModAlt);
        CHECK(decode("\033R", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "r" && c.mods == (sonora::kModAlt | sonora::kModShift));
        CHECK(decode("\033#", c) == TerminalInput::Result::Chord);
        CHECK(c.key == "3" && c.mods == (sonora::kModAlt | sonora::kModShift));

        // A lone Escape is only an Escape once nothing more is coming.
        {
            TerminalInput in;
            const char esc = '\033';
            in.feed(&esc, 1);
            CHECK(in.next(false, c) == TerminalInput::Result::NeedMore);
            CHECK(in.next(true, c) == TerminalInput::Result::Chord);
            CHECK(c.key == "escape" && c.mods == sonora::kModNone);
            CHECK(in.next(true, c) == TerminalInput::Result::None);
        }

        // A sequence split across two reads still arrives as one chord, and
        // produces nothing at all in the meantime.
        {
            TerminalInput in;
            in.feed("\033[", 2);
            CHECK(in.next(false, c) == TerminalInput::Result::NeedMore);
            in.feed("A", 1);
            CHECK(in.next(false, c) == TerminalInput::Result::Chord);
            CHECK(c.key == "up");
        }

        // A sequence we have no name for is consumed whole. Letting its tail
        // through as key presses is the defect.
        CHECK(decode("\033[?1;2c", c) == TerminalInput::Result::Unrecognised);
        {
            TerminalInput in;
            in.feed("\033[200~x", 7);
            KeyChord first;
            CHECK(in.next(true, first) == TerminalInput::Result::Unrecognised);
            CHECK(in.next(true, first) == TerminalInput::Result::Chord);
            CHECK(first.key == "x");
        }

        // Several key presses in one read all come out, in order.
        {
            TerminalInput in;
            in.feed("q\033[Aa", 5);
            CHECK(in.next(false, c) == TerminalInput::Result::Chord);
            CHECK(c.key == "q");
            CHECK(in.next(false, c) == TerminalInput::Result::Chord);
            CHECK(c.key == "up");
            CHECK(in.next(false, c) == TerminalInput::Result::Chord);
            CHECK(c.key == "a");
            CHECK(in.next(false, c) == TerminalInput::Result::None);
        }
    }

    // --- the dispatcher ---
    {
        CommandRegistry reg;
        int armed = -1;
        int quits = 0;

        CHECK(reg.install("track.arm", [&](const CommandContext& ctx) {
            armed = ctx.target;
            return true;
        }));
        CHECK(reg.install("app.quit", [&](const CommandContext&) {
            ++quits;
            return true;
        }));
        CHECK(reg.install("edit.undo", [](const CommandContext&) {
            return false;  // nothing to undo
        }));

        // A handler for a command outside the vocabulary is rejected at
        // install time, so a typo in the wiring cannot hide until a key press.
        CHECK(!reg.install("track.armm", [](const CommandContext&) { return true; }));

        CommandContext ctx;
        ctx.target = 4;
        CHECK(reg.dispatch("track.arm", ctx) == DispatchResult::Ok);
        CHECK(armed == 4);
        CHECK(reg.dispatch("app.quit") == DispatchResult::Ok);
        CHECK(quits == 1);
        CHECK(reg.dispatch("edit.undo") == DispatchResult::Failed);

        // In the catalog, nothing wired up: reportable, never silent.
        CHECK(!reg.implemented("edit.mode.slip"));
        CHECK(reg.dispatch("edit.mode.slip") == DispatchResult::NotImplemented);
        CHECK(reg.dispatch("no.such.command") == DispatchResult::UnknownCommand);
        CHECK(std::string(dispatch_result_text(DispatchResult::NotImplemented)) ==
              "not implemented in this build");
    }

    // --- the catalog itself ---
    {
        CHECK(sonora::find_command("transport.toggle_record") != nullptr);
        CHECK(sonora::find_command("transport.toggle_recrod") == nullptr);
        CHECK(sonora::find_command("track.arm")->target ==
              sonora::CommandTarget::Track);
        CHECK(sonora::find_command("app.quit")->target ==
              sonora::CommandTarget::None);

        // No duplicate ids: a duplicate would make find_command's answer
        // depend on table order.
        const auto& cat = sonora::command_catalog();
        for (std::size_t i = 0; i < cat.size(); ++i) {
            for (std::size_t j = i + 1; j < cat.size(); ++j) {
                if (std::string(cat[i].id) == cat[j].id) {
                    std::fprintf(stderr, "FAIL duplicate command id %s\n",
                                 cat[i].id);
                    return 1;
                }
            }
        }
    }

    // --- a target out of range is refused, not narrowed ---
    //
    // 4294967297 used to load and store target 0, so the key armed track 1:
    // a plausible file, no error, the wrong track. That is the shape of the
    // port-ordering defect and it does not get to happen twice.
    {
        KeyMap map;
        std::string err;
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.toggle_arm", "target": 4294967297 } } })",
                     map, err));
        CHECK(mentions(err, "from 1 to"));
        CHECK(map.bindings().empty());

        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.toggle_arm", "target": 1025 } } })",
                     map, err));
        CHECK(mentions(err, "from 1 to"));

        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.toggle_arm", "target": 2.5 } } })",
                     map, err));
        CHECK(mentions(err, "whole track number"));

        // 1e999 is infinity once parsed, and casting an infinity to a long is
        // undefined rather than merely wrong. It has to fail in the number
        // reader, before anything narrows it.
        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.toggle_arm", "target": 1e999 } } })",
                     map, err));
        CHECK(mentions(err, "too large to represent"));

        err.clear();
        CHECK(!loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.toggle_arm", "target": -1e999 } } })",
                     map, err));
        CHECK(mentions(err, "too large to represent"));

        // The upper bound itself still loads, so the message is not a lie.
        err.clear();
        CHECK(loads(R"({ "name": "x", "bindings": {
            "q": { "command": "track.toggle_arm", "target": 1024 } } })",
                    map, err));
        CHECK(map.resolve("q")->ctx.target == 1023);
    }

    // --- deep nesting is a parse error, not a crash ---
    //
    // value() -> object()/array() -> value() recursed with no limit, and a
    // file nested 50 000 deep exited on SIGSEGV. A bad error message beats a
    // signal; a signal tells the operator nothing at all.
    {
        KeyMap map;
        std::string err;
        for (const int depth : {100, 5000, 50000, 100000}) {
            std::string json = R"({ "name": "x", "bindings": { "q": )";
            json.append(static_cast<std::size_t>(depth), '[');
            json.append(static_cast<std::size_t>(depth), ']');
            json += " } }";
            err.clear();
            CHECK(!loads(json, map, err));
            CHECK(mentions(err, "nesting deeper than"));
            CHECK(mentions(err, "<test>:1"));
        }
        // And the limit is above anything a key map needs: a binding object is
        // three levels deep.
        err.clear();
        CHECK(loads(R"({ "name": "x", "bindings": {
            "q": { "command": "app.quit" } } })", map, err));
    }

    // --- \u escapes ---
    {
        KeyMap map;
        std::string err;
        CHECK(loads("{ \"name\": \"A\\u0042C\", \"bindings\": {} }", map, err));
        CHECK(map.name() == "ABC");

        // Two bytes for U+00E9, three for U+20AC.
        CHECK(loads("{ \"name\": \"caf\\u00e9\", \"bindings\": {} }", map, err));
        CHECK(map.name() == "caf\xc3\xa9");
        CHECK(loads("{ \"name\": \"\\u20ac\", \"bindings\": {} }", map, err));
        CHECK(map.name() == "\xe2\x82\xac");

        // A surrogate pair is the only way JSON can write U+1F3B9, and a
        // comment with an emoji in it is a legitimate thing for a profile to
        // carry. Four bytes of UTF-8 out.
        CHECK(loads("{ \"name\": \"x\", \"comment\": \"\\ud83c\\udfb9\","
                    " \"bindings\": {} }", map, err));
        CHECK(map.comment() == "\xf0\x9f\x8e\xb9");

        // A surrogate on its own is not a character. Refused either way round,
        // rather than encoded into ill-formed UTF-8.
        err.clear();
        CHECK(!loads("{ \"name\": \"\\ud83c\", \"bindings\": {} }", map, err));
        CHECK(mentions(err, "no trailing surrogate"));

        err.clear();
        CHECK(!loads("{ \"name\": \"\\udfb9\", \"bindings\": {} }", map, err));
        CHECK(mentions(err, "no leading surrogate"));

        err.clear();
        CHECK(!loads("{ \"name\": \"\\ud83c\\u0041\", \"bindings\": {} }",
                     map, err));
        CHECK(mentions(err, "not a" " trailing surrogate"));

        err.clear();
        CHECK(!loads("{ \"name\": \"\\ud8\", \"bindings\": {} }", map, err));
        CHECK(mentions(err, "non-hex digit"));

        // Only a real end of input is "truncated"; a non-hex byte has its own
        // message, above.
        err.clear();
        CHECK(!loads("{ \"name\": \"\\u00", map, err));
        CHECK(mentions(err, "truncated"));
    }

    // --- load_file reports a bad file on disk, not only a bad string ---
    {
        const std::string path = "test_keymap_malformed.json";
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("{\n  \"name\": \"Bad\",\n  \"bindings\": {\n"
                   "    \"space\": \"transport.nope\"\n  }\n}\n", f);
        std::fclose(f);

        KeyMap map;
        std::string err;
        CHECK(!KeyMap::load_file(path, map, err));
        CHECK(mentions(err, path.c_str()));   // the file, by name
        CHECK(mentions(err, ":4"));           // the line
        CHECK(mentions(err, "transport.nope"));
        CHECK(map.bindings().empty());
        std::remove(path.c_str());

        err.clear();
        CHECK(!KeyMap::load_file("no_such_keymap_anywhere.json", map, err));
        CHECK(mentions(err, "cannot be opened"));
    }

    // --- every binding in every shipped profile round trips ---
    //
    // This is the test that makes a typo in a profile a CI failure instead of
    // a key that does nothing during a take.
    {
        const char* profiles[] = {"default.json", "protools.json", "logic.json"};
        int unverified = 0;
        for (const char* file : profiles) {
            const std::string path = std::string(SONORA_KEYMAP_DIR) + "/" + file;
            KeyMap map;
            std::string err;
            if (!KeyMap::load_file(path, map, err)) {
                std::fprintf(stderr, "FAIL %s does not load: %s\n", path.c_str(),
                             err.c_str());
                return 1;
            }
            if (map.bindings().empty()) {
                std::fprintf(stderr, "FAIL %s has no bindings\n", path.c_str());
                return 1;
            }
            for (const Binding& b : map.bindings()) {
                // No check that find_command() knows b.command here: load
                // already refuses the whole file for that, so the check could
                // never fire. The test that an unknown command is a load
                // error is above, on a string.
                const Binding* found = map.resolve(b.chord);
                if (found == nullptr || found->command != b.command ||
                    found->ctx.target != b.ctx.target) {
                    std::fprintf(stderr, "FAIL %s: %s does not resolve back\n",
                                 file, b.spec.c_str());
                    return 1;
                }
                // A chord must survive being written out and read back, or a
                // key map editor would corrupt the file it saved.
                KeyChord again;
                std::string why;
                if (!KeyMap::parse_chord(KeyMap::chord_to_string(b.chord), again,
                                         why) ||
                    again != b.chord) {
                    std::fprintf(stderr, "FAIL %s: chord %s does not round"
                                 " trip (%s)\n", file, b.spec.c_str(),
                                 why.c_str());
                    return 1;
                }
                if (!b.verified) ++unverified;
            }
        }
        // Not a failure, but it must be visible: unverified bindings are a
        // debt that someone has to check against the real applications.
        std::printf("test_keymap: %d unverified bindings across the profiles\n",
                    unverified);
    }

    // --- the built-in default is the shipped default ---
    {
        const KeyMap built = KeyMap::built_in_default();
        KeyMap on_disk;
        std::string err;
        const std::string path = std::string(SONORA_KEYMAP_DIR) + "/default.json";
        if (!KeyMap::load_file(path, on_disk, err)) {
            std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        CHECK(built.name() == on_disk.name());
        CHECK(built.bindings().size() == on_disk.bindings().size());
        for (const Binding& b : on_disk.bindings()) {
            const Binding* m = built.resolve(b.chord);
            if (m == nullptr || m->command != b.command ||
                m->ctx.target != b.ctx.target) {
                std::fprintf(stderr,
                             "FAIL keymaps/default.json and the built-in map"
                             " disagree about %s\n", b.spec.c_str());
                return 1;
            }
        }
        // And it still covers exactly what main.cpp's loop has always done.
        CHECK(built.resolve("space")->command == "transport.toggle_record");
        CHECK(built.resolve("a")->command == "track.arm_all");
        CHECK(built.resolve("q")->command == "app.quit");
        for (int i = 1; i <= 8; ++i) {
            const Binding* b = built.resolve(std::string(1, static_cast<char>('0' + i)));
            CHECK(b != nullptr);
            CHECK(b->command == "track.toggle_arm");
            CHECK(b->ctx.target == i - 1);
        }
    }

    // --- every shipped profile can record from a terminal ---
    //
    // This is the test that makes the blocker impossible to reintroduce.
    // protools.json and logic.json both loaded cleanly, round-tripped every
    // chord and passed CI while being unable to arm a single track at a tty,
    // because "shift+1" arrives as "!" and nothing read it back.
    {
        const char* profiles[] = {"default.json", "protools.json", "logic.json"};
        const std::vector<KeyChord> device = KeyMap::terminal_chords();

        for (const char* file : profiles) {
            const std::string path = std::string(SONORA_KEYMAP_DIR) + "/" + file;
            KeyMap map;
            std::string err;
            if (!KeyMap::load_file(path, map, err)) {
                std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), err.c_str());
                return 1;
            }

            // The sweep the reviewer ran: all 256 bytes a tty can deliver,
            // through chord_from_terminal_byte and resolve. At least one of
            // them must arm a track, or the profile cannot record.
            int arms_from_a_byte = 0;
            for (int b = 0; b < 256; ++b) {
                KeyChord c;
                if (!KeyMap::chord_from_terminal_byte(
                        static_cast<unsigned char>(b), c)) {
                    continue;
                }
                const Binding* bound = map.resolve(c);
                if (bound == nullptr) continue;
                if (bound->command == "track.toggle_arm" ||
                    bound->command == "track.arm" ||
                    bound->command == "track.arm_all") {
                    ++arms_from_a_byte;
                }
            }
            if (arms_from_a_byte == 0) {
                std::fprintf(stderr,
                             "FAIL %s: no single byte a terminal can send arms"
                             " a track, so this profile cannot record\n", file);
                return 1;
            }

            // The same question over the whole input device, including the
            // sequences a single byte cannot carry: arming must need no
            // repair in a profile we ship. Quitting is deliberately not held
            // to that -- logic.json puts it on cmd+q, which is correct for a
            // Mac application and which no terminal can send, so the honest
            // answer there is the start-up notice, not a CI failure.
            if (!satisfied(map, KeyMap::essential_commands()[0], device,
                           anything_is_implemented)) {
                std::fprintf(stderr,
                             "FAIL %s: nothing this terminal can send arms a"
                             " track\n", file);
                return 1;
            }

            // And with only the handlers this build actually has, the engine
            // must end up able to arm, start and stop a take, and quit --
            // after repair, and having said so.
            KeyMap repaired = map;
            const std::vector<std::string> notices = repaired.ensure_essentials(
                device, implemented);
            for (const auto& group : KeyMap::essential_commands()) {
                if (satisfied(repaired, group, device, implemented)) continue;
                std::fprintf(stderr,
                             "FAIL %s: still cannot reach %s after repair\n",
                             file, group[0].c_str());
                return 1;
            }
            // A repair must never leave two bindings on one chord, which is
            // the invariant load_string enforces and resolve() relies on.
            for (std::size_t i = 0; i < repaired.bindings().size(); ++i) {
                for (std::size_t j = i + 1; j < repaired.bindings().size(); ++j) {
                    if (repaired.bindings()[i].chord ==
                        repaired.bindings()[j].chord) {
                        std::fprintf(stderr,
                                     "FAIL %s: repair duplicated the chord %s\n",
                                     file, repaired.bindings()[i].spec.c_str());
                        return 1;
                    }
                }
            }
            for (const std::string& n : notices) {
                std::printf("test_keymap: %s: %s\n", file, n.c_str());
            }
        }

        // Exactly the repairs each shipped profile needs, so a change to a
        // profile or to the handlers that alters them is a decision someone
        // makes rather than a notice nobody reads.
        //
        // Pro Tools starts a take on ctrl+space and f12 (transport.record)
        // and stops it on space, which is transport.toggle_play -- and there
        // is no playback to toggle. So space becomes transport.stop: it stops
        // a take as Pro Tools' space does, and does nothing where Pro Tools
        // would play, rather than starting a take nobody asked for. ctrl+q
        // needs no repair now that flow control is off. Logic is the same for
        // space, with r as its record key, and its quit is cmd+q, which no
        // terminal can send, so q is bound to app.quit.
        {
            const char* const space_stops =
                "\"space\" -> transport.stop (was transport.toggle_play)";
            struct Expected {
                const char* file;
                std::size_t notices;
                std::vector<const char*> says;
                const char* space;  // what space runs after repair
            };
            const Expected expected[] = {
                {"default.json", 0, {}, "transport.toggle_record"},
                {"protools.json", 1, {space_stops}, "transport.stop"},
                {"logic.json", 2, {space_stops, "\"q\" -> app.quit"},
                 "transport.stop"},
            };
            for (const Expected& e : expected) {
                KeyMap map;
                std::string err;
                CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/" +
                                            e.file, map, err));
                const std::vector<std::string> notices =
                    map.ensure_essentials(device, implemented);
                bool said = true;
                for (const char* want : e.says) {
                    bool found = false;
                    for (const std::string& n : notices) {
                        found = found || mentions(n, want);
                    }
                    if (!found) {
                        std::fprintf(stderr, "FAIL %s: no notice says %s\n",
                                     e.file, want);
                        said = false;
                    }
                }
                if (notices.size() != e.notices || !said) {
                    std::fprintf(stderr, "FAIL %s: %zu notices, expected %zu\n",
                                 e.file, notices.size(), e.notices);
                    for (const std::string& n : notices) {
                        std::fprintf(stderr, "  %s\n", n.c_str());
                    }
                    return 1;
                }
                CHECK(map.resolve("space") != nullptr);
                CHECK(map.resolve("space")->command == e.space);
            }
        }

        // The default profile needs no repair at all: it is the fallback
        // everything else is repaired from, so a notice here means the
        // fallback itself is broken.
        KeyMap built = KeyMap::built_in_default();
        CHECK(built.ensure_essentials(device, implemented).empty());

        // A profile that binds nothing a terminal can send gets told so,
        // repaired, and ends up able to record.
        {
            KeyMap stuck;
            std::string err;
            CHECK(loads(R"({ "name": "Mouse only", "bindings": {
                "cmd+r": "track.arm_all",
                "cmd+space": "transport.toggle_record",
                "cmd+q": "app.quit" } })", stuck, err));
            const std::vector<std::string> notices =
                stuck.ensure_essentials(device, implemented);
            // Arm, start and quit. Stopping needs no notice of its own: the
            // key bound to start a take is a toggle and stops it too.
            CHECK(notices.size() == 3);
            for (const auto& group : KeyMap::essential_commands()) {
                CHECK(satisfied(stuck, group, device, implemented));
            }
            // The cmd+... bindings are still there; nothing was removed.
            CHECK(stuck.resolve("cmd+q") != nullptr);
        }
    }

    std::printf("test_keymap: ok\n");
    return 0;
}
