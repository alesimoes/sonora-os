// SPDX-License-Identifier: GPL-3.0-or-later
//
// Qt key events through ui/keymap: one key system, so a profile means the
// same thing in the window as at the tty, and each shipped profile can arm,
// record, stop and quit from a Qt keyboard with what this build implements.
#include <cstdio>
#include <string>

#include "key_router.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "synthetic_engine.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

static std::string chord(int key, Qt::KeyboardModifiers m = Qt::NoModifier) {
    KeyChord c;
    if (chord_from_qt_key(key, m, false, c) != QtKeyDecode::Chord) return "<none>";
    return KeyMap::chord_to_string(c);
}

static bool device_has(const std::vector<KeyChord>& d, const std::string& spec) {
    KeyChord c;
    std::string err;
    if (!KeyMap::parse_chord(spec, c, err)) return false;
    for (const auto& x : d) {
        if (x == c) return true;
    }
    return false;
}

int main() {
    // --- decoding ---
    {
        CHECK(chord(Qt::Key_Space) == "space");
        CHECK(chord(Qt::Key_Space, Qt::ControlModifier) == "ctrl+space");
        CHECK(chord(Qt::Key_A) == "a");
        CHECK(chord(Qt::Key_A, Qt::ShiftModifier) == "shift+a");
        CHECK(chord(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier) == "ctrl+shift+z");
        CHECK(chord(Qt::Key_Q, Qt::MetaModifier) == "cmd+q");
        CHECK(chord(Qt::Key_7, Qt::ControlModifier) == "ctrl+7");
        CHECK(chord(Qt::Key_F12) == "f12");
        CHECK(chord(Qt::Key_Backtab, Qt::ShiftModifier) == "shift+tab");
        // Shifted digits read as the tty reads them: "#" is shift+3, which
        // is what protools.json's arming keys and alt+shift+3 are written as.
        CHECK(chord(Qt::Key_NumberSign, Qt::ShiftModifier) == "shift+3");
        CHECK(chord(Qt::Key_NumberSign, Qt::AltModifier | Qt::ShiftModifier) == "alt+shift+3");
        CHECK(chord(Qt::Key_Exclam, Qt::ShiftModifier) == "shift+1");
        CHECK(chord(Qt::Key_1, Qt::ShiftModifier) == "shift+1");  // some paths report the base key
        CHECK(chord(Qt::Key_BracketLeft, Qt::ControlModifier) == "ctrl+bracketleft");
        CHECK(chord(Qt::Key_BraceLeft, Qt::ShiftModifier) == "shift+bracketleft");
        CHECK(chord(Qt::Key_Plus, Qt::ShiftModifier) == "plus");  // as at the tty
        CHECK(chord(Qt::Key_Semicolon) == "semicolon");
        CHECK(chord(Qt::Key_QuoteLeft) == "grave");
        // The keypad is its own keys, which the tty could never say.
        CHECK(chord(Qt::Key_3, Qt::KeypadModifier) == "numpad_3");
        CHECK(chord(Qt::Key_Plus, Qt::KeypadModifier) == "numpad_plus");
        CHECK(chord(Qt::Key_Asterisk, Qt::KeypadModifier) == "numpad_multiply");
        CHECK(chord(Qt::Key_Enter, Qt::KeypadModifier) == "numpad_enter");
        CHECK(chord(Qt::Key_End, Qt::KeypadModifier) == "numpad_1");  // Num Lock off
        CHECK(chord(Qt::Key_End) == "end");

        KeyChord c;
        CHECK(chord_from_qt_key(Qt::Key_Shift, Qt::ShiftModifier, false, c) ==
              QtKeyDecode::ModifierOnly);
        CHECK(chord_from_qt_key(Qt::Key_Control, Qt::ControlModifier, false, c) ==
              QtKeyDecode::ModifierOnly);
        // Holding Space must not start and stop takes at the repeat rate.
        CHECK(chord_from_qt_key(Qt::Key_Space, Qt::NoModifier, true, c) ==
              QtKeyDecode::AutoRepeat);
        CHECK(chord_from_qt_key(Qt::Key_VolumeUp, Qt::NoModifier, false, c) ==
              QtKeyDecode::Unrecognised);
    }

    // --- the device: what a Qt keyboard can send ---
    const std::vector<KeyChord> device = qt_device_chords();
    {
        CHECK(device_has(device, "cmd+q"));
        CHECK(device_has(device, "ctrl+shift+z"));
        CHECK(device_has(device, "numpad_3"));
        CHECK(device_has(device, "ctrl+alt+bracketleft"));
        CHECK(device_has(device, "shift+8"));
        // The compositor keeps these for VT switching.
        CHECK(!device_has(device, "ctrl+alt+f1"));
        CHECK(!device_has(device, "ctrl+alt+f12"));
        CHECK(device_has(device, "ctrl+f1"));
        // Every chord the decoder names is one ui/keymap can parse back.
        for (const auto& c : device) {
            KeyChord back;
            std::string err;
            CHECK(KeyMap::parse_chord(KeyMap::chord_to_string(c), back, err));
            CHECK(back == c);
        }
    }

    // --- each shipped profile through the router, against the engine's
    //     implemented commands plus the interface's app.quit ---
    const std::string dir = SONORA_KEYMAP_DIR;
    for (const char* file : {"default.json", "protools.json", "logic.json"}) {
        KeyMap map;
        std::string err;
        CHECK(KeyMap::load_file(dir + "/" + file, map, err));

        test::RecordingAdapter engine(18, SyntheticEngine::engine_commands());
        KeyRouter router(map, engine.info().implemented_commands,
                         [&engine](const std::string& c, const CommandContext& ctx) {
                             return engine.send(c, ctx);
                         });
        bool quit = false;
        router.local().install("app.quit", [&quit](const CommandContext&) {
            quit = true;
            return true;
        });
        const std::vector<std::string> notices = router.prepare(device);
        for (const auto& n : notices) std::printf("  %s: %s\n", file, n.c_str());
        // Every essential group is reachable from a Qt keyboard after repair.
        for (const auto& g : KeyMap::essential_commands()) {
            CHECK(router.map().reaches(g, device,
                                       [&router](const std::string& id) {
                                           return router.implemented(id);
                                       }));
        }
        CHECK(!router.help().empty());
        std::printf("  %s: %zu footer entries, %zu notices\n", file, router.help().size(),
                    notices.size());
    }

    // --- protools.json: the keys an engineer's hands already know ---
    {
        KeyMap map;
        std::string err;
        CHECK(KeyMap::load_file(dir + "/protools.json", map, err));
        test::RecordingAdapter engine(18, SyntheticEngine::engine_commands());
        KeyRouter router(map, engine.info().implemented_commands,
                         [&engine](const std::string& c, const CommandContext& ctx) {
                             return engine.send(c, ctx);
                         });
        router.prepare(device);

        const auto press = [&](int key, Qt::KeyboardModifiers m) {
            KeyChord c;
            chord_from_qt_key(key, m, false, c);
            return router.route(c);
        };

        // Shift+3 on a US keyboard arrives as "#" with Shift: arms track 3.
        RouteResult r = press(Qt::Key_NumberSign, Qt::ShiftModifier);
        CHECK(r.kind == RouteResult::Kind::Sent);
        CHECK(engine.sent.size() == 1);
        CHECK(engine.sent[0].command == "track.toggle_arm");
        CHECK(engine.sent[0].target == 2);

        // Ctrl+Space and F12 start a take.
        r = press(Qt::Key_Space, Qt::ControlModifier);
        CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.record");
        r = press(Qt::Key_F12, Qt::NoModifier);
        CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.record");
        // Space is Pro Tools' play/stop; with no playback it was repaired to
        // transport.stop, exactly as at the tty, so it never starts a take.
        r = press(Qt::Key_Space, Qt::NoModifier);
        CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.stop");

        // A catalogued command nobody implements is said out loud, and never
        // reaches the engine.
        const std::size_t before = engine.sent.size();
        r = press(Qt::Key_Z, Qt::ControlModifier);  // edit.undo
        CHECK(r.kind == RouteResult::Kind::NotImplemented);
        CHECK(r.message.find("not implemented") != std::string::npos);
        CHECK(engine.sent.size() == before);

        // An unbound chord says so.
        r = press(Qt::Key_J, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        CHECK(r.kind == RouteResult::Kind::Unbound);
        CHECK(!r.message.empty());

        // A click on an arm button goes exactly where the key goes.
        CommandContext ctx;
        ctx.target = 5;
        r = router.invoke("track.toggle_arm", ctx);
        CHECK(r.kind == RouteResult::Kind::Sent);
        CHECK(engine.sent.back().command == "track.toggle_arm");
        CHECK(engine.sent.back().target == 5);
    }

    // --- logic.json: Cmd is a real modifier in the window ---
    {
        KeyMap map;
        std::string err;
        CHECK(KeyMap::load_file(dir + "/logic.json", map, err));
        test::RecordingAdapter engine(18, SyntheticEngine::engine_commands());
        KeyRouter router(map, engine.info().implemented_commands,
                         [&engine](const std::string& c, const CommandContext& ctx) {
                             return engine.send(c, ctx);
                         });
        bool quit = false;
        router.local().install("app.quit", [&quit](const CommandContext&) {
            quit = true;
            return true;
        });
        const auto notices = router.prepare(device);
        // At the tty logic.json's quit is unreachable and q is borrowed from
        // the default. Here cmd+q is Logic's own key, so nothing is borrowed
        // for it.
        for (const auto& n : notices) CHECK(n.find("app.quit") == std::string::npos);
        KeyChord c;
        chord_from_qt_key(Qt::Key_Q, Qt::MetaModifier, false, c);
        const RouteResult r = router.route(c);
        CHECK(r.kind == RouteResult::Kind::Local);
        CHECK(quit);
        // "r" is Logic's record.
        chord_from_qt_key(Qt::Key_R, Qt::NoModifier, false, c);
        CHECK(router.route(c).command == "transport.record");
    }

    std::printf("ui keys: all checks passed\n");
    return 0;
}
