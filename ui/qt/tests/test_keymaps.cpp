// SPDX-License-Identifier: GPL-3.0-or-later
//
// Key maps driving the control API (Track 3 item 4):
//
//   - commands go where their CommandSpec::domain says: Interface commands
//     never reach the engine; an Engine command goes to the engine once it
//     says it implements it, through FakeEngine's wire, and the window's
//     stand-in is then no longer used;
//   - Pro Tools' Commands Keyboard Focus: the letters run only with it on,
//     say so with it off, and ctrl+alt+1 or the a-z button toggles it, on
//     US, AZERTY and ABNT2 keyboards alike;
//   - the profile switch at run time, what it repairs, what it refuses and
//     what it remembers for the next start;
//   - the Keys menu and the a-z button in the real QML, offscreen;
//   - an engine restarted with another command list routes by its own
//     Hello, not the first one; with another format and track count, what
//     was sized from the first Hello follows, and the start-up refusal
//     notice survives the essentials' notices changing;
//   - profile paths compared normalised, a relative $XDG_CONFIG_HOME
//     ignored, and the start-up fallback when the remembered profile no
//     longer loads.
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "control_adapter.h"
#include "key_router.h"
#include "keymap_profiles.h"
#include "meter_item.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "timeline_document.h"

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

const std::string kDir = SONORA_KEYMAP_DIR;

KeyMap load(const std::string& file) {
    KeyMap m;
    std::string err;
    if (!KeyMap::load_file(kDir + "/" + file, m, err)) {
        std::fprintf(stderr, "FAIL %s: %s\n", file.c_str(), err.c_str());
    }
    return m;
}

KeyChord chord(const std::string& spec) {
    KeyChord c;
    std::string err;
    KeyMap::parse_chord(spec, c, err);
    return c;
}

// XKB keycodes: Linux evdev codes plus 8.
quint32 xkb_digit(int d) { return d == 0 ? 19 : 9 + static_cast<quint32>(d); }

// A key as the xcb and Wayland platforms deliver it.
KeyChord qt_chord(int key, Qt::KeyboardModifiers m, quint32 xkb, const QString& text) {
    const QKeyEvent e(QEvent::KeyPress, key, m, xkb, 0, 0, text);
    KeyChord c;
    chord_from_qt_key(qt_key_input(e, true), c);
    return c;
}

void key(QQuickView& view, int k, Qt::KeyboardModifiers m, quint32 xkb = 0,
         const QString& text = QString()) {
    QKeyEvent press(QEvent::KeyPress, k, m, xkb, 0, 0, text);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m, xkb, 0, 0, text);
    QCoreApplication::sendEvent(&view, &release);
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// --- domains ---------------------------------------------------------------

int test_domains() {
    // The engine lists an Interface command and an Engine command the
    // window also has a stand-in for.
    test::RecordingAdapter engine(4, {"tool.smart", "edit.undo", "transport.toggle_play"});
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    int undo_here = 0;
    router.local().install("edit.undo", [&undo_here](const CommandContext&) {
        ++undo_here;
        return true;
    });
    router.prepare(qt_device_chords(true));

    // tool.smart is the interface's: listed by the engine, never sent, and
    // with no handler here it is said to be missing.
    RouteResult r = router.route(chord("ctrl+7"));
    CHECK(r.kind == RouteResult::Kind::NotImplemented);
    CHECK(engine.sent.empty());
    CHECK(!router.implemented("tool.smart"));

    // edit.undo is the engine's, and the engine runs it: the window's
    // stand-in is not used.
    r = router.route(chord("ctrl+z"));
    CHECK(r.kind == RouteResult::Kind::Sent);
    CHECK(engine.sent.size() == 1 && engine.sent[0].command == "edit.undo");
    CHECK(undo_here == 0);

    // An engine that does not list it: the stand-in runs.
    test::RecordingAdapter older(4, {"transport.toggle_play"});
    KeyRouter r2(load("protools.json"), older.info().implemented_commands,
                 [&older](const std::string& c, const CommandContext& ctx) {
                     return older.send(c, ctx);
                 });
    r2.local().install("edit.undo", [&undo_here](const CommandContext&) {
        ++undo_here;
        return true;
    });
    r2.prepare(qt_device_chords(true));
    r = r2.route(chord("ctrl+z"));
    CHECK(r.kind == RouteResult::Kind::Local && r.result == DispatchResult::Ok);
    CHECK(undo_here == 1 && older.sent.empty());
    return 0;
}

// --- planned engine commands, on FakeEngine's wire --------------------------

int test_planned_on_fake_engine() {
    // What the profiles bind and the engine does not run yet (BACKLOG
    // Track 1), announced by the fake so the keys can be wired now.
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    cfg.planned_commands = {"transport.go_to_end", "transport.rewind",
                            "transport.play_half_speed", "tool.smart", "no.such"};
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    const auto& impl = engine.info().implemented_commands;
    CHECK(has(impl, "transport.go_to_end") && has(impl, "transport.rewind"));
    // Interface commands and unknown ids are never announced.
    CHECK(!has(impl, "tool.smart") && !has(impl, "no.such"));

    KeyRouter router(load("protools.json"), impl,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    int selected = 2;
    router.setSelection([&selected]() { return selected; });
    router.prepare(qt_device_chords(true));

    // Option+Return (Pro Tools' Go to end of session), keypad 1 (rewind),
    // Shift+Space (half-speed playback). Shift+I left this list when the
    // interface took track.toggle_input_monitor (test_edit).
    CHECK(router.route(chord("alt+return")).kind == RouteResult::Kind::Sent);
    CHECK(router.route(chord("numpad_1")).kind == RouteResult::Kind::Sent);
    CHECK(router.route(chord("shift+space")).kind == RouteResult::Kind::Sent);
    // Space is a command the engine really runs.
    CHECK(router.route(chord("space")).kind == RouteResult::Kind::Sent);
    std::vector<CommandOutcome> replies;
    engine.poll(replies);
    CHECK(replies.size() == 4);
    for (const auto& o : replies) CHECK(o.result == DispatchResult::Ok);
    const auto& log = fake.planned_log();
    CHECK(log.size() == 3);
    CHECK(log[0].command == "transport.go_to_end" && log[0].target == -1);
    CHECK(log[1].command == "transport.rewind");
    CHECK(log[2].command == "transport.play_half_speed" && log[2].target == -1);

    // On the fake as --fake-engine runs it, the same keys are said to be
    // missing and send nothing.
    api::testing::FakeEngine plain;
    ControlAdapter plain_engine(plain);
    CHECK(plain_engine.connect(err));
    KeyRouter plain_router(load("protools.json"), plain_engine.info().implemented_commands,
                           [&plain_engine](const std::string& c, const CommandContext& ctx) {
                               return plain_engine.send(c, ctx);
                           });
    plain_router.prepare(qt_device_chords(true));
    const RouteResult r = plain_router.route(chord("alt+return"));
    CHECK(r.kind == RouteResult::Kind::NotImplemented);
    CHECK(r.message.find("not implemented") != std::string::npos);
    return 0;
}

// --- Commands Keyboard Focus ----------------------------------------------

int test_focus() {
    test::RecordingAdapter engine(4, api::testing::FakeEngine::engine_commands());
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    int trims = 0;
    router.local().install("edit.trim_start_to_insertion", [&trims](const CommandContext&) {
        ++trims;
        return true;
    });
    router.prepare(qt_device_chords(true));
    CHECK(router.hasFocusKeys());
    CHECK(router.focusToggleChord() == "ctrl+alt+1");

    // On at start, as the letters always were.
    CHECK(router.commandsFocus());
    CHECK(has(router.help(), "a=trim_start_to_insertion"));
    RouteResult r = router.route(chord("a"));
    CHECK(r.kind == RouteResult::Kind::Local && trims == 1);

    // Ctrl+Alt+1 turns it off, on every layout: the digit is the key, read
    // from its XKB keycode, whatever the key types.
    const KeyChord us = qt_chord(Qt::Key_1, Qt::ControlModifier | Qt::AltModifier,
                                 xkb_digit(1), QStringLiteral("1"));
    const KeyChord azerty = qt_chord(Qt::Key_Ampersand, Qt::ControlModifier | Qt::AltModifier,
                                     xkb_digit(1), QStringLiteral("&"));
    const KeyChord abnt2 = qt_chord(Qt::Key_1, Qt::ControlModifier | Qt::AltModifier,
                                    xkb_digit(1), QStringLiteral("1"));
    CHECK(KeyMap::chord_to_string(us) == "ctrl+alt+1");
    CHECK(azerty == us && abnt2 == us);

    r = router.route(us);
    CHECK(r.kind == RouteResult::Kind::Local && r.command == "focus.commands");
    CHECK(!router.commandsFocus());
    CHECK(!has(router.help(), "a=trim_start_to_insertion"));
    // The focus-free form still trims: Option+Shift+7.
    CHECK(has(router.help(), "alt+shift+7=trim_start_to_insertion"));

    // With focus off the letter does not run, and says why and how.
    r = router.route(chord("a"));
    CHECK(r.kind == RouteResult::Kind::FocusOff);
    CHECK(trims == 1);
    CHECK(r.message.find("Commands Keyboard Focus") != std::string::npos);
    CHECK(r.message.find("ctrl+alt+1") != std::string::npos);
    std::printf("  focus off: %s\n", r.message.c_str());
    r = router.route(chord("alt+shift+7"));
    CHECK(r.kind == RouteResult::Kind::Local && trims == 2);
    // Keys that are not focus keys are unaffected.
    const std::size_t before = engine.sent.size();
    CHECK(router.route(chord("space")).kind == RouteResult::Kind::Sent);
    CHECK(engine.sent.size() == before + 1);

    // AZERTY's key turns it on again; so would the a-z button (invoke).
    CHECK(router.route(azerty).kind == RouteResult::Kind::Local);
    CHECK(router.commandsFocus());
    CHECK(router.invoke("focus.commands", CommandContext{}).kind == RouteResult::Kind::Local);
    CHECK(!router.commandsFocus());
    router.setCommandsFocus(true);

    // Logic has no focus keys: nothing for an a-z button to change.
    KeyRouter logic(load("logic.json"), engine.info().implemented_commands,
                    [&engine](const std::string& c, const CommandContext& ctx) {
                        return engine.send(c, ctx);
                    });
    logic.prepare(qt_device_chords(true));
    CHECK(!logic.hasFocusKeys());
    CHECK(logic.focusToggleChord().empty());
    return 0;
}

// --- the profile switch ----------------------------------------------------

int test_switch() {
    test::RecordingAdapter engine(4, api::testing::FakeEngine::engine_commands());
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.local().install("app.quit", [](const CommandContext&) { return true; });
    router.prepare(qt_device_chords(true));
    router.setCommandsFocus(false);
    CHECK(router.route(chord("f12")).command == "transport.record");

    // To Logic: R records, F12 is nobody's, and the focus is the operator's.
    std::vector<std::string> notices = router.setMap(load("logic.json"));
    CHECK(router.map().name() == "Logic Pro");
    CHECK(router.route(chord("r")).command == "transport.record");
    CHECK(router.route(chord("f12")).kind == RouteResult::Kind::Unbound);
    CHECK(!router.commandsFocus());
    for (const auto& g : KeyMap::essential_commands()) {
        CHECK(router.map().reaches(g, qt_device_chords(true), [&router](const std::string& id) {
            return router.implemented(id);
        }));
    }
    for (const auto& n : notices) std::printf("  logic.json: %s\n", n.c_str());

    // A profile with no quit key is repaired on switch as at start-up.
    KeyMap no_quit;
    std::string err;
    CHECK(KeyMap::load_string(R"({"name": "No quit", "bindings": {
        "space": "transport.toggle_record",
        "1": { "command": "track.toggle_arm", "target": 1 }
    }})", "t", no_quit, err));
    notices = router.setMap(no_quit);
    CHECK(notices.size() == 1 && notices[0].find("app.quit") != std::string::npos);
    CHECK(router.route(chord("q")).command == "app.quit");
    return 0;
}

// --- the profile list and what is remembered -------------------------------

int test_profiles() {
    std::vector<ProfileEntry> list = list_profiles(kDir);
    // keymaps/default.json is the built-in map, so it is listed once.
    CHECK(list.size() == 3);
    CHECK(list[0].name == "Default" && !list[0].file.empty());
    CHECK(list[1].name == "Logic Pro" && list[2].name == "Pro Tools");
    for (const auto& e : list) CHECK(e.error.empty());

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const std::string dir = tmp.filePath(QStringLiteral("keys")).toStdString();
    QDir().mkpath(QString::fromStdString(dir));
    {
        std::ofstream(dir + "/bad.json") << R"({"name": "Bad", "bindings": {"space": "nope"}})";
        std::ofstream(dir + "/mine.json")
            << R"({"name": "Mine", "bindings": {"space": "transport.toggle_play"}})";
        std::ofstream(dir + "/notes.txt") << "not a profile";
    }
    list = list_profiles(dir);
    // No default.json here: the built-in default first, then the files.
    CHECK(list.size() == 3);
    CHECK(list[0].file.empty() && list[0].name == "Default");
    CHECK(list[1].name == "Mine" && list[1].error.empty());
    CHECK(list[2].name == "bad.json" && list[2].error.find("nope") != std::string::npos);
    // A --keymap outside the folder is listed too.
    list = list_profiles(dir, kDir + "/logic.json");
    CHECK(list.size() == 4);
    CHECK(list_profiles(tmp.filePath(QStringLiteral("absent")).toStdString()).size() == 1);

    // Remembered as a bare name inside the folder, read back absolute.
    const std::string at = remembered_profile_path(tmp.filePath(QStringLiteral("config")).toStdString());
    CHECK(at.find("sonora") != std::string::npos);
    CHECK(remembered_profile(at, dir).empty());
    std::string why;
    CHECK(remember_profile(at, dir, dir + "/mine.json", why));
    {
        std::ifstream in(at);
        std::string line;
        std::getline(in, line);
        CHECK(line == "mine.json");
    }
    CHECK(remembered_profile(at, dir) == (std::filesystem::path(dir) / "mine.json").string());
    // Outside the folder: the whole path.
    CHECK(remember_profile(at, dir, kDir + "/logic.json", why));
    CHECK(remembered_profile(at, dir) == kDir + "/logic.json");
    // The built-in default: nothing.
    CHECK(remember_profile(at, dir, "", why));
    CHECK(remembered_profile(at, dir).empty());

    // A config folder that cannot be made says why.
    const std::string blocker = tmp.filePath(QStringLiteral("file")).toStdString();
    std::ofstream(blocker) << "x";
    const std::string blocked = remembered_profile_path(blocker);
    CHECK(!remember_profile(blocked, dir, dir + "/mine.json", why));
    CHECK(!why.empty());
    std::printf("  unwritable: %s\n", why.c_str());

    // The folder search: --keymap-dir, then --keymap's folder.
    CHECK(keymap_dir(dir, "", "") == dir);
    CHECK(keymap_dir(tmp.filePath(QStringLiteral("absent")).toStdString(), "", "").empty());
    qunsetenv("SONORA_KEYMAP_DIR");
    CHECK(keymap_dir("", kDir + "/protools.json", "") == kDir);
    return 0;
}

// --- the shell: the Keys menu and the a-z button ------------------------------

int test_shell() {
    test::RecordingAdapter engine(18, api::testing::FakeEngine::engine_commands());
    ModelDocument document(make_demo_session(18, 48000));
    SyntheticPeakSource peaks(48000, 120.0);
    SessionModel session(document, peaks, true);
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.local().install("app.quit", [](const CommandContext&) { return true; });
    session.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));

    ShellModel model(engine, router);
    model.setScanCodesAreXkb(true);
    model.addNotice(QStringLiteral("engine notice"));
    model.setKeymapNotices({QStringLiteral("old key map notice")});
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    std::vector<std::string> remembered;
    bool remember_ok = true;
    model.setProfiles(list_profiles(kDir), kDir + "/protools.json",
                      [&](const std::string& file, std::string& why) {
                          remembered.push_back(file);
                          if (!remember_ok) why = "read-only configuration";
                          return remember_ok;
                      });

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    model.routeKeysFrom(&view);

    QQuickItem* root = view.rootObject();
    QQuickItem* focus = named(root, QStringLiteral("focusButton"));
    QQuickItem* keys = named(root, QStringLiteral("keysButton"));
    QQuickItem* panel = named(root, QStringLiteral("profilePanel"));
    CHECK(focus && keys && panel);
    CHECK(focus->isVisible());
    CHECK(focus->property("active").toBool());
    CHECK(keys->property("text").toString() == QStringLiteral("Keys: Pro Tools"));
    CHECK(!panel->isVisible());

    // The current profile is marked in the list.
    const QVariantList profiles = model.profiles();
    CHECK(profiles.size() == 3);
    int current = 0;
    for (const QVariant& p : profiles) current += p.toMap().value(QStringLiteral("current")).toBool();
    CHECK(current == 1);

    // Ctrl+Alt+1 from the keyboard: focus off, the button follows.
    key(view, Qt::Key_1, Qt::ControlModifier | Qt::AltModifier, xkb_digit(1), QStringLiteral("1"));
    CHECK(!model.commandsFocus());
    CHECK(!focus->property("active").toBool());
    // A focus letter now says so on the status line.
    key(view, Qt::Key_A, Qt::NoModifier, 38, QStringLiteral("a"));
    CHECK(model.statusText().contains(QStringLiteral("Commands Keyboard Focus")));
    // The a-z button sends what Ctrl+Alt+1 sends.
    QMetaObject::invokeMethod(focus, "clicked", Q_ARG(QVariant, QVariant()));
    CHECK(model.commandsFocus());
    CHECK(focus->property("active").toBool());

    // The Keys button opens the menu.
    QMetaObject::invokeMethod(keys, "clicked", Q_ARG(QVariant, QVariant()));
    CHECK(panel->isVisible());

    // Switch to Logic: the name, the a-z button gone (no focus keys), the
    // old key map notice replaced and the engine's kept, the choice
    // remembered, and Logic's R recording.
    model.useProfile(QString::fromStdString(kDir + "/logic.json"));
    CHECK(model.profileName() == QStringLiteral("Logic Pro"));
    CHECK(keys->property("text").toString() == QStringLiteral("Keys: Logic Pro"));
    CHECK(!focus->isVisible());
    CHECK(model.notices().contains(QStringLiteral("engine notice")));
    CHECK(!model.notices().contains(QStringLiteral("old key map notice")));
    CHECK(remembered.size() == 1 && remembered[0] == kDir + "/logic.json");
    const std::size_t before = engine.sent.size();
    key(view, Qt::Key_R, Qt::NoModifier, 27, QStringLiteral("r"));
    CHECK(engine.sent.size() == before + 1 && engine.sent.back().command == "transport.record");

    // A file that stopped loading since the list was made: refused, keys
    // unchanged, the reason on the status line and in the list.
    const QString broken = tmp.filePath(QStringLiteral("broken.json"));
    {
        QFile f(broken);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(R"({"name": "Broken", "bindings": {"space": "transport.nope"}})");
    }
    model.useProfile(broken);
    CHECK(model.profileName() == QStringLiteral("Logic Pro"));
    CHECK(model.statusText().contains(QStringLiteral("refused")));
    CHECK(remembered.size() == 1);

    // A choice that cannot be remembered is said, and still used now.
    remember_ok = false;
    model.useProfile(QString::fromStdString(kDir + "/protools.json"));
    CHECK(model.profileName() == QStringLiteral("Pro Tools"));
    bool said = false;
    for (const QString& n : model.notices()) said = said || n.contains(QStringLiteral("read-only"));
    CHECK(said);
    CHECK(focus->isVisible());

    // The built-in default ("" file) loads without a file.
    remember_ok = true;
    model.useProfile(QString());
    CHECK(model.profileName() == QStringLiteral("Default"));
    CHECK(remembered.back().empty());
    return 0;
}

// --- a restarted engine: its Hello, not the first one, decides -------------

// An engine that can be replaced under the adapter, as a restarted engine
// is under SocketEngine: a new greeting and the new engine's Hello.
class RestartableEngine final : public api::EngineControl {
public:
    explicit RestartableEngine(api::testing::FakeEngine* e) : e_(e) {}
    void restart(api::testing::FakeEngine* e) {
        e_ = e;
        ++greetings;
    }
    bool connect(std::string& error) override { return e_->connect(error); }
    const api::Hello& hello() const override { return e_->hello(); }
    uint64_t send(const api::CommandRequest& r) override { return e_->send(r); }
    uint64_t send(const api::ParameterChange& c) override { return e_->send(c); }
    uint64_t send(const api::PluginParameterChange& c) override { return e_->send(c); }
    uint64_t submit(const api::ModelRequest& r) override { return e_->submit(r); }
    void poll(api::Inbox& inbox) override { e_->poll(inbox); }
    api::StateView& state() override { return e_->state(); }
    uint64_t greetings = 1;

private:
    api::testing::FakeEngine* e_;
};

int test_engine_restart() {
    // The first engine runs edit.undo; the one it is restarted as does not.
    api::testing::FakeEngine::Config with_undo;
    with_undo.tracks = 4;
    with_undo.planned_commands = {"edit.undo"};
    api::testing::FakeEngine first(with_undo);
    api::testing::FakeEngine::Config plain_cfg;
    plain_cfg.tracks = 4;
    api::testing::FakeEngine second(plain_cfg);
    std::string err;
    CHECK(second.connect(err));

    RestartableEngine backend(&first);
    ControlAdapter engine(backend);
    CHECK(engine.connect(err));
    CHECK(has(engine.info().implemented_commands, "edit.undo"));
    engine.watchConnection([&backend]() { return backend.greetings; }, [](bool) {});
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    int undo_here = 0;
    router.local().install("edit.undo", [&undo_here](const CommandContext&) {
        ++undo_here;
        return true;
    });
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);

    CHECK(router.route(chord("ctrl+z")).kind == RouteResult::Kind::Sent);
    CHECK(undo_here == 0);

    // Restarted without it: the window's stand-in runs again, instead of
    // the key being sent to an engine that answers NotImplemented.
    backend.restart(&second);
    model.tick();
    CHECK(!has(engine.info().implemented_commands, "edit.undo"));
    const RouteResult r = router.route(chord("ctrl+z"));
    CHECK(r.kind == RouteResult::Kind::Local && r.result == DispatchResult::Ok);
    CHECK(undo_here == 1);
    CHECK(has(router.help(), "ctrl+z=undo"));

    // And back: the engine that runs it is again the one that does.
    backend.restart(&first);
    model.tick();
    CHECK(router.route(chord("ctrl+z")).kind == RouteResult::Kind::Sent);
    CHECK(undo_here == 1);
    return 0;
}

// A restarted engine with another format and track count (PR #67 review):
// what was sized from the first Hello follows the new one. Before, the
// track count, the format line and the arm buttons stayed the first
// engine's, the clock read the new playhead at the old rate, and a meter on
// a track the first engine lacked showed nothing, which looks like silence.
int test_engine_restart_format() {
    uint64_t now = 1'000'000'000;
    const auto clock = [&now]() { return now; };
    api::testing::FakeEngine::Config small;
    small.tracks = 4;
    small.clock = clock;
    api::testing::FakeEngine first(small);
    api::testing::FakeEngine::Config big;
    big.tracks = 8;
    big.sample_rate = 96000;
    big.buffer_frames = 256;
    big.clock = clock;
    api::testing::FakeEngine second(big);
    std::string err;
    CHECK(second.connect(err));

    RestartableEngine backend(&first);
    ControlAdapter engine(backend);
    CHECK(engine.connect(err));
    engine.watchConnection([&backend]() { return backend.greetings; }, [](bool) {});
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    model.setClock(clock);
    int format_signals = 0;
    QObject::connect(&model, &ShellModel::engineFormatChanged,
                     [&format_signals]() { ++format_signals; });
    MeterItem meter;   // after the model: detaches before the model goes
    meter.setChannel(6);
    model.attachMeter(&meter);
    const auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            now += 10'000'000;
            model.tick();
        }
    };
    ticks(4);
    CHECK(model.trackCount() == 4);
    CHECK(model.formatText() == QStringLiteral("48000 Hz / 128 frames"));
    CHECK(model.armed().size() == 4);
    CHECK(!meter.ballistics().has_data);
    CHECK(format_signals == 0);

    backend.restart(&second);
    ticks(4);
    CHECK(format_signals == 1);
    CHECK(model.trackCount() == 8);
    CHECK(model.formatText() == QStringLiteral("96000 Hz / 256 frames"));
    CHECK(model.armed().size() == 8);
    CHECK(meter.ballistics().has_data);
    // The clock reads the new engine's playhead at the new engine's rate:
    // one second rolled is one second, not two.
    model.invoke(QStringLiteral("transport.play"), -1);
    ticks(1);
    now += 1'000'000'000;   // one second after the play was handled
    for (int i = 0; i < 4; ++i) model.tick();
    CHECK(model.transportText() == QStringLiteral("PLAY"));
    CHECK(model.clockText() == QStringLiteral("00:00:01.000"));

    // The same Hello again (a reconnect to the same engine) is not a change.
    backend.restart(&second);
    ticks(4);
    CHECK(format_signals == 1);
    return 0;
}

// The start-up refusal ("the remembered profile no longer loads") is a
// profile notice; a new engine's Hello replaces the essentials' notices
// alone, so the refusal must survive a reconnect that changes them.
int test_restart_keeps_refusal() {
    // Arming only through track.arm on a fixed track (a key needing a
    // selection does not count): the first engine runs it, the second does
    // not, so the second's Hello brings an essentials repair.
    api::testing::FakeEngine::Config with_arm;
    with_arm.tracks = 4;
    with_arm.planned_commands = {"track.arm"};
    api::testing::FakeEngine first(with_arm);
    api::testing::FakeEngine::Config plain_cfg;
    plain_cfg.tracks = 4;
    api::testing::FakeEngine second(plain_cfg);
    std::string err;
    CHECK(second.connect(err));

    KeyMap map;
    CHECK(KeyMap::load_string(R"({"name": "Arm by track.arm", "bindings": {
        "shift+a": {"command": "track.arm", "target": 1},
        "ctrl+space": "transport.toggle_record",
        "space": "transport.toggle_play", "ctrl+q": "app.quit"}})",
                              "arm.json", map, err));
    RestartableEngine backend(&first);
    ControlAdapter engine(backend);
    CHECK(engine.connect(err));
    engine.watchConnection([&backend]() { return backend.greetings; }, [](bool) {});
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.local().install("app.quit", [](const CommandContext&) { return true; });
    const std::vector<std::string> at_start = router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    const QString refusal =
        QStringLiteral("the key map remembered from last time was refused: gone.json");
    QStringList essentials;
    for (const auto& n : at_start) essentials << QString::fromStdString(n);
    model.setKeymapNotices({refusal}, essentials);
    const QStringList before = model.notices();
    CHECK(before.contains(refusal));

    backend.restart(&second);
    model.tick();
    CHECK(model.notices() != before);   // the essentials' notices changed
    CHECK(model.notices().contains(refusal));
    CHECK(model.notices().count(refusal) == 1);

    backend.restart(&first);
    model.tick();
    CHECK(model.notices() == before);
    return 0;
}

// --- profile paths, the XDG folder, the start-up fallback ---------------------

int test_profile_paths() {
    // --keymap-dir with ./ and .. in it: the current profile is still marked.
    const std::string odd_dir = kDir + "/../keymaps/.";
    const std::string odd_file = kDir + "/./../keymaps/protools.json";
    const std::vector<ProfileEntry> list = list_profiles(odd_dir);
    CHECK(list.size() == 3);
    for (const auto& e : list) {
        CHECK(e.file.find("/./") == std::string::npos && e.file.find("/../") == std::string::npos);
    }
    CHECK(profile_path(odd_file) == profile_path(kDir + "/protools.json"));
    CHECK(profile_path("").empty());

    test::RecordingAdapter engine(4, api::testing::FakeEngine::engine_commands());
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    model.setProfiles(list, odd_file, nullptr);
    int current = 0;
    for (const QVariant& p : model.profiles()) {
        const QVariantMap m = p.toMap();
        if (m.value(QStringLiteral("current")).toBool()) {
            ++current;
            CHECK(m.value(QStringLiteral("name")).toString() == QStringLiteral("Pro Tools"));
        }
    }
    CHECK(current == 1);

    // A relative $XDG_CONFIG_HOME is ignored, as the XDG spec says: under
    // systemd it would put the choice under /.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QByteArray saved_xdg = qgetenv("XDG_CONFIG_HOME");
    const QByteArray saved_home = qgetenv("HOME");
    const bool had_xdg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const std::string home = tmp.path().toStdString();
    qputenv("HOME", tmp.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", "relative/config");
    const std::string from_relative = remembered_profile_path();
    qputenv("XDG_CONFIG_HOME", (tmp.path() + QStringLiteral("/xdg")).toUtf8());
    const std::string from_absolute = remembered_profile_path();
    qputenv("HOME", saved_home);
    if (had_xdg) {
        qputenv("XDG_CONFIG_HOME", saved_xdg);
    } else {
        qunsetenv("XDG_CONFIG_HOME");
    }
    CHECK(from_relative == (std::filesystem::path(home) / ".config" / "sonora" / "keymap").string());
    CHECK(from_absolute == (std::filesystem::path(home) / "xdg" / "sonora" / "keymap").string());
    return 0;
}

int test_startup_fallback() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const std::string dir = tmp.filePath(QStringLiteral("keys")).toStdString();
    QDir().mkpath(QString::fromStdString(dir));
    {
        std::ofstream(dir + "/mine.json")
            << R"({"name": "Mine", "bindings": {"space": "transport.toggle_play"}})";
    }
    const std::string at =
        remembered_profile_path(tmp.filePath(QStringLiteral("config")).toStdString());
    std::string why;
    CHECK(remember_profile(at, dir, dir + "/mine.json", why));

    // Remembered and still loading: it is what starts.
    StartupProfile s = startup_profile("", at, dir);
    CHECK(s.refusal.empty());
    CHECK(s.map.name() == "Mine");
    CHECK(s.file == profile_path(dir + "/mine.json"));

    // The remembered file no longer loads: the built-in keys, the refusal
    // naming the reason, and no file marked as current.
    {
        std::ofstream(dir + "/mine.json", std::ios::trunc)
            << R"({"name": "Mine", "bindings": {"space": "transport.gone"}})";
    }
    s = startup_profile("", at, dir);
    CHECK(s.map.name() == KeyMap::built_in_default().name());
    CHECK(s.file.empty());
    CHECK(s.refusal.find("refused") != std::string::npos);
    CHECK(s.refusal.find("transport.gone") != std::string::npos);
    std::printf("  remembered, no longer loads: %s\n", s.refusal.c_str());

    // Gone altogether: the same.
    std::filesystem::remove(dir + "/mine.json");
    s = startup_profile("", at, dir);
    CHECK(s.map.name() == KeyMap::built_in_default().name());
    CHECK(s.file.empty() && !s.refusal.empty());

    // --keymap wins over what is remembered.
    s = startup_profile(kDir + "/./logic.json", at, dir);
    CHECK(s.refusal.empty() && s.map.name() == "Logic Pro");
    CHECK(s.file == profile_path(kDir + "/logic.json"));

    // Nothing remembered, no --keymap: the built-in keys, and no refusal.
    CHECK(remember_profile(at, dir, "", why));
    s = startup_profile("", at, dir);
    CHECK(s.refusal.empty() && s.file.empty());
    CHECK(s.map.name() == KeyMap::built_in_default().name());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_domains() != 0) return 1;
    if (test_planned_on_fake_engine() != 0) return 1;
    if (test_focus() != 0) return 1;
    if (test_switch() != 0) return 1;
    if (test_profiles() != 0) return 1;
    if (test_shell() != 0) return 1;
    if (test_engine_restart() != 0) return 1;
    if (test_engine_restart_format() != 0) return 1;
    if (test_restart_keeps_refusal() != 0) return 1;
    if (test_profile_paths() != 0) return 1;
    if (test_startup_fallback() != 0) return 1;
    std::printf("ui keymaps: all checks passed\n");
    return 0;
}
