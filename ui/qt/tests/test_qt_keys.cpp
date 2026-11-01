// SPDX-License-Identifier: GPL-3.0-or-later
//
// Qt key events through ui/keymap: one key system, so a profile means the
// same thing in the window as at the tty, and each shipped profile can arm,
// record, stop and quit from a Qt keyboard with what this build implements.
//
// Layout cases are synthetic key events built as Qt's xcb and Wayland
// plugins deliver them: Qt's key from the layout, the modifiers, the XKB
// keycode of the physical key, the native modifier mask and the text.
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <cstdio>
#include <string>

#include "key_router.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "sonora/api/fake_engine.h"

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

namespace {

// XKB keycodes: Linux evdev codes plus 8.
quint32 xkb_digit(int d) { return d == 0 ? 19 : 9 + static_cast<quint32>(d); }
constexpr quint32 kXkbQ = 24;     // AD01: "a" on AZERTY
constexpr quint32 kXkbW = 25;     // AD02: "z" on AZERTY
constexpr quint32 kXkbAD11 = 34;  // dead ^ on AZERTY, dead acute on ABNT2
constexpr quint32 kXkbAC10 = 47;  // ";" on US, "m" on AZERTY, "c cedilla" on ABNT2
constexpr quint32 kXkbZ = 52;     // AB01
constexpr quint32 kXkbAE11 = 20;  // "-" on US, ")" on AZERTY
constexpr quint32 kXkbBKSL = 51;  // "\" on US, "*" on AZERTY
constexpr quint32 kXkbAB08 = 59;  // "," on US, ";" on AZERTY
constexpr quint32 kXkbAB10 = 61;  // "/" on US, "!" on AZERTY
constexpr quint32 kMod5 = 0x80;   // AltGr

std::string decode(const QtKeyInput& in) {
    KeyChord c;
    if (chord_from_qt_key(in, c) != QtKeyDecode::Chord) return "<none>";
    return KeyMap::chord_to_string(c);
}

// A layout-independent key, where Qt's key is all there is to read.
std::string chord(int key, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QtKeyInput in;
    in.key = key;
    in.mods = m;
    return decode(in);
}

// A key event as a platform delivers it.
std::string event(int key, Qt::KeyboardModifiers m, quint32 xkb, quint32 native_mods,
                  const QString& text, bool xkb_platform = true) {
    const QKeyEvent e(QEvent::KeyPress, key, m, xkb, 0, native_mods, text);
    return decode(qt_key_input(e, xkb_platform));
}

KeyChord chord_of(int key, Qt::KeyboardModifiers m, quint32 xkb = 0) {
    QtKeyInput in;
    in.key = key;
    in.mods = m;
    in.xkb_keycode = xkb;
    KeyChord c;
    chord_from_qt_key(in, c);
    return c;
}

bool device_has(const std::vector<KeyChord>& d, const std::string& spec) {
    KeyChord c;
    std::string err;
    if (!KeyMap::parse_chord(spec, c, err)) return false;
    for (const auto& x : d) {
        if (x == c) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);  // QKeyEvent needs the input devices

    const Qt::KeyboardModifiers none = Qt::NoModifier;
    const Qt::KeyboardModifiers shift = Qt::ShiftModifier;

    // --- layout-independent keys ---
    {
        CHECK(chord(Qt::Key_Space) == "space");
        CHECK(chord(Qt::Key_Space, Qt::ControlModifier) == "ctrl+space");
        CHECK(chord(Qt::Key_A) == "a");
        CHECK(chord(Qt::Key_A, Qt::ShiftModifier) == "shift+a");
        CHECK(chord(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier) == "ctrl+shift+z");
        CHECK(chord(Qt::Key_Q, Qt::MetaModifier) == "cmd+q");
        CHECK(chord(Qt::Key_F12) == "f12");
        CHECK(chord(Qt::Key_Backtab, Qt::ShiftModifier) == "shift+tab");
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
        QtKeyInput in;
        in.key = Qt::Key_Shift;
        in.mods = Qt::ShiftModifier;
        CHECK(chord_from_qt_key(in, c) == QtKeyDecode::ModifierOnly);
        in.key = Qt::Key_AltGr;
        in.mods = Qt::GroupSwitchModifier;
        CHECK(chord_from_qt_key(in, c) == QtKeyDecode::ModifierOnly);
        // Holding Space must not start and stop takes at the repeat rate.
        in.key = Qt::Key_Space;
        in.mods = none;
        in.auto_repeat = true;
        CHECK(chord_from_qt_key(in, c) == QtKeyDecode::AutoRepeat);
        in.key = Qt::Key_VolumeUp;
        in.auto_repeat = false;
        CHECK(chord_from_qt_key(in, c) == QtKeyDecode::Unrecognised);
    }

    // --- US: the digit row by its key ---
    {
        CHECK(event(Qt::Key_1, none, xkb_digit(1), 0, QStringLiteral("1")) == "1");
        CHECK(event(Qt::Key_Exclam, shift, xkb_digit(1), 0, QStringLiteral("!")) == "shift+1");
        CHECK(event(Qt::Key_NumberSign, shift, xkb_digit(3), 0, QStringLiteral("#")) ==
              "shift+3");
        CHECK(event(Qt::Key_NumberSign, Qt::AltModifier | shift, xkb_digit(3), 0,
                    QStringLiteral("#")) == "alt+shift+3");
        CHECK(event(Qt::Key_ParenRight, shift, xkb_digit(0), 0, QStringLiteral(")")) ==
              "shift+0");
        CHECK(event(Qt::Key_7, Qt::ControlModifier, xkb_digit(7), 0, QString()) == "ctrl+7");
        CHECK(event(Qt::Key_Z, Qt::ControlModifier, kXkbZ, 0, QString()) == "ctrl+z");
        CHECK(event(Qt::Key_Comma, none, kXkbAB08, 0, QStringLiteral(",")) == "comma");
    }

    // --- AZERTY: the digit row types symbols unshifted, digits shifted ---
    {
        // The defect: "&" read the US way is shift+7, which armed track 7.
        CHECK(event(Qt::Key_Ampersand, none, xkb_digit(1), 0, QStringLiteral("&")) == "1");
        CHECK(event(Qt::Key_1, shift, xkb_digit(1), 0, QStringLiteral("1")) == "shift+1");
        CHECK(event(Qt::Key_Eacute, none, xkb_digit(2), 0, QStringLiteral("é")) == "2");
        CHECK(event(Qt::Key_QuoteDbl, none, xkb_digit(3), 0, QStringLiteral("\"")) == "3");
        CHECK(event(Qt::Key_3, shift, xkb_digit(3), 0, QStringLiteral("3")) == "shift+3");
        CHECK(event(Qt::Key_Minus, none, xkb_digit(6), 0, QStringLiteral("-")) == "6");
        CHECK(event(Qt::Key_7, Qt::ControlModifier, xkb_digit(7), 0, QString()) == "ctrl+7");
        // AltGr+3 types "#", which is not shift+3: Wayland says AltGr only in
        // the native mask, xcb in GroupSwitchModifier as well.
        CHECK(event(Qt::Key_NumberSign, none, xkb_digit(3), kMod5, QStringLiteral("#")) ==
              "<none>");
        CHECK(event(Qt::Key_NumberSign, Qt::GroupSwitchModifier, xkb_digit(3), kMod5,
                    QStringLiteral("#")) == "<none>");
        // Shifted-digit characters typed off the digit row mean nothing.
        CHECK(event(Qt::Key_ParenRight, none, kXkbAE11, 0, QStringLiteral(")")) == "<none>");
        CHECK(event(Qt::Key_Asterisk, none, kXkbBKSL, 0, QStringLiteral("*")) == "<none>");
        CHECK(event(Qt::Key_Exclam, none, kXkbAB10, 0, QStringLiteral("!")) == "<none>");
        // Letters are the letters on the keys.
        CHECK(event(Qt::Key_A, none, kXkbQ, 0, QStringLiteral("a")) == "a");
        CHECK(event(Qt::Key_Z, Qt::ControlModifier, kXkbW, 0, QString()) == "ctrl+z");
        CHECK(event(Qt::Key_M, none, kXkbAC10, 0, QStringLiteral("m")) == "m");
        // Other punctuation by its character, as at the tty.
        CHECK(event(Qt::Key_Semicolon, none, kXkbAB08, 0, QStringLiteral(";")) ==
              "semicolon");
        // A dead key off the digit row.
        CHECK(event(Qt::Key_Dead_Circumflex, none, kXkbAD11, 0, QString()) == "<none>");
    }

    // --- ABNT2 ---
    {
        CHECK(event(Qt::Key_1, none, xkb_digit(1), 0, QStringLiteral("1")) == "1");
        CHECK(event(Qt::Key_Exclam, shift, xkb_digit(1), 0, QStringLiteral("!")) == "shift+1");
        // Shift+6 is a dead diaeresis: no text, and still the key with 6 on it.
        CHECK(event(Qt::Key_Dead_Diaeresis, shift, xkb_digit(6), 0, QString()) == "shift+6");
        // AltGr+1 types a superscript one.
        CHECK(event(Qt::Key_onesuperior, none, xkb_digit(1), kMod5, QStringLiteral("¹")) ==
              "<none>");
        CHECK(event(Qt::Key_twosuperior, Qt::GroupSwitchModifier, xkb_digit(2), kMod5,
                    QStringLiteral("²")) == "<none>");
        // AltGr+Q types "/", which is not the slash key.
        CHECK(event(Qt::Key_Slash, none, kXkbQ, kMod5, QStringLiteral("/")) == "<none>");
        CHECK(event(Qt::Key_Dead_Acute, none, kXkbAD11, 0, QString()) == "<none>");
        CHECK(event(Qt::Key_Ccedilla, none, kXkbAC10, 0, QStringLiteral("ç")) == "<none>");
        CHECK(event(Qt::Key_Q, none, kXkbQ, 0, QStringLiteral("q")) == "q");
    }

    // --- a layout with no Latin letters: the letter at the key's US place ---
    CHECK(event(0x042f, Qt::ControlModifier, kXkbZ, 0, QString()) == "ctrl+z");  // Cyrillic Ya

    // --- a platform whose scan codes are not XKB keycodes ---
    {
        CHECK(event(Qt::Key_NumberSign, shift, xkb_digit(3), 0, QStringLiteral("#"), false) ==
              "<none>");
        CHECK(event(Qt::Key_1, none, xkb_digit(1), 0, QStringLiteral("1"), false) == "<none>");
        CHECK(event(Qt::Key_A, none, kXkbQ, 0, QStringLiteral("a"), false) == "a");
        CHECK(event(Qt::Key_Space, none, 65, 0, QStringLiteral(" "), false) == "space");
        CHECK(qt_scan_codes_are_xkb(QStringLiteral("xcb")));
        CHECK(qt_scan_codes_are_xkb(QStringLiteral("wayland")));
        CHECK(qt_scan_codes_are_xkb(QStringLiteral("wayland-egl")));
        CHECK(!qt_scan_codes_are_xkb(QStringLiteral("eglfs")));
        CHECK(!qt_scan_codes_are_xkb(QStringLiteral("offscreen")));
        const std::vector<KeyChord> d = qt_device_chords(false);
        CHECK(!device_has(d, "shift+1"));
        CHECK(!device_has(d, "1"));
        CHECK(device_has(d, "numpad_1"));
        CHECK(device_has(d, "a"));
    }

    // --- the device: what a Qt keyboard can send ---
    const std::vector<KeyChord> device = qt_device_chords(true);
    {
        CHECK(device_has(device, "cmd+q"));
        CHECK(device_has(device, "ctrl+shift+z"));
        CHECK(device_has(device, "numpad_3"));
        CHECK(device_has(device, "ctrl+alt+bracketleft"));
        CHECK(device_has(device, "shift+8"));
        CHECK(device_has(device, "ctrl+7"));
        CHECK(device_has(device, "ctrl+shift+1"));
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

        test::RecordingAdapter engine(18, api::testing::FakeEngine::engine_commands());
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
        test::RecordingAdapter engine(18, api::testing::FakeEngine::engine_commands());
        KeyRouter router(map, engine.info().implemented_commands,
                         [&engine](const std::string& c, const CommandContext& ctx) {
                             return engine.send(c, ctx);
                         });
        router.prepare(device);

        const auto press = [&](int key, Qt::KeyboardModifiers m, quint32 xkb = 0) {
            return router.route(chord_of(key, m, xkb));
        };

        // Shift+3 on a US keyboard arrives as "#" with Shift: arms track 3.
        RouteResult r = press(Qt::Key_NumberSign, shift, xkb_digit(3));
        CHECK(r.kind == RouteResult::Kind::Sent);
        CHECK(engine.sent.size() == 1);
        CHECK(engine.sent[0].command == "track.toggle_arm");
        CHECK(engine.sent[0].target == 2);

        // AZERTY: the unshifted "1/&" key is "1", which Pro Tools does not
        // bind -- not shift+7, which arms track 7.
        r = press(Qt::Key_Ampersand, none, xkb_digit(1));
        CHECK(r.kind == RouteResult::Kind::Unbound);
        CHECK(engine.sent.size() == 1);
        // ...and Shift on it arms track 1, as on a US keyboard.
        r = press(Qt::Key_1, shift, xkb_digit(1));
        CHECK(r.kind == RouteResult::Kind::Sent);
        CHECK(engine.sent.size() == 2 && engine.sent[1].target == 0);
        // ABNT2: Shift+6 is a dead key, and still arms track 6.
        r = press(Qt::Key_Dead_Diaeresis, shift, xkb_digit(6));
        CHECK(r.kind == RouteResult::Kind::Sent);
        CHECK(engine.sent.size() == 3 && engine.sent[2].target == 5);

        // F12 makes the transport record-ready, and Ctrl+Space is a Play
        // that then starts the take (ADR-0009 update of 2026-11-01).
        r = press(Qt::Key_Space, Qt::ControlModifier);
        CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.play");
        r = press(Qt::Key_F12, none);
        CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.record_ready");
        // Space is Pro Tools' play/stop, and the engine plays back: it plays,
        // and stops a roll or a take, exactly as at the tty.
        r = press(Qt::Key_Space, none);
        CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.toggle_play");

        // A catalogued command nobody implements is said out loud, and never
        // reaches the engine.
        const std::size_t before = engine.sent.size();
        r = press(Qt::Key_Z, Qt::ControlModifier);  // edit.undo
        CHECK(r.kind == RouteResult::Kind::NotImplemented);
        CHECK(r.message.find("not implemented") != std::string::npos);
        CHECK(engine.sent.size() == before);

        // An unbound chord says so.
        r = press(Qt::Key_J, Qt::ControlModifier | Qt::AltModifier | shift);
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
        test::RecordingAdapter engine(18, api::testing::FakeEngine::engine_commands());
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
        const RouteResult r = router.route(chord_of(Qt::Key_Q, Qt::MetaModifier));
        CHECK(r.kind == RouteResult::Kind::Local);
        CHECK(quit);
        // "r" is Logic's record.
        CHECK(router.route(chord_of(Qt::Key_R, none)).command == "transport.record");
    }

    // --- a refused profile: the built-in keys, and a line naming why ---
    {
        const std::string builtin = KeyMap::built_in_default().name();
        std::string refusal = "stale";
        KeyMap m = keymap_or_built_in("", refusal);
        CHECK(m.name() == builtin);
        CHECK(refusal.empty());

        m = keymap_or_built_in(dir + "/protools.json", refusal);
        CHECK(m.name() != builtin);
        CHECK(refusal.empty());

        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString bad = tmp.filePath(QStringLiteral("bad.json"));
        QFile f(bad);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(R"({"name": "Bad", "bindings": {"space": "transport.no_such_command"}})");
        f.close();
        m = keymap_or_built_in(bad.toStdString(), refusal);
        CHECK(m.name() == builtin);
        CHECK(refusal.find("refused") != std::string::npos);
        CHECK(refusal.find(bad.toStdString()) != std::string::npos);
        CHECK(refusal.find("no_such_command") != std::string::npos);
        std::printf("  refused: %s\n", refusal.c_str());

        const std::string missing = tmp.filePath(QStringLiteral("absent.json")).toStdString();
        m = keymap_or_built_in(missing, refusal);
        CHECK(m.name() == builtin);
        CHECK(refusal.find(missing) != std::string::npos);
    }

    std::printf("ui keys: all checks passed\n");
    return 0;
}
