// SPDX-License-Identifier: GPL-3.0-or-later
//
// The terminal mode the interface reads keys in (tty_mode.h), measured as
// the profile sees it. Moved here from ui/keymap's test in M4: termios is
// this module's business, and ui/keymap's test is now platform-neutral
// (ADR-0008, V4).
#include "sonora/tty_mode.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "sonora/api/fake_engine.h"
#include "sonora/keymap.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::KeyChord;
using sonora::KeyMap;
using Implemented = std::function<bool(const std::string&)>;

bool loads(const std::string& json, KeyMap& out, std::string& error) {
    return KeyMap::load_string(json, "<test>", out, error);
}

bool sendable(const std::vector<KeyChord>& device, const KeyChord& c) {
    return std::find(device.begin(), device.end(), c) != device.end();
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
// that -- the operator had no keyboard way out at all. run_terminal() then switches
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

}  // namespace

int main() {
    // The engine's commands as its Hello announces them, and app.quit, the
    // interface's own.
    sonora::api::testing::FakeEngine engine;
    std::string error;
    if (!engine.connect(error)) {
        std::fprintf(stderr, "FAIL the fake engine did not connect: %s\n", error.c_str());
        return 1;
    }
    const Implemented implemented = [&engine](const std::string& id) {
        if (id == "app.quit") return true;
        const auto& ids = engine.hello().implemented_commands;
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    int failures = 0;
    failures += test_key_reading_mode();
    failures += test_interrupt_key_when_nothing_quits(implemented);
    if (failures != 0) {
        std::fprintf(stderr, "FAIL %d tty mode tests\n", failures);
        return 1;
    }
    std::printf("tty mode: all tests passed\n");
    return 0;
}
