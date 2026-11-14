// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Preferences, in the real QML over FakeEngine's wire: the Setup
// menu opens the window through window.preferences; a choice applies at once
// and is kept in the file, so the next start finds it; a file this version
// does not understand and a file that cannot be written are refused, loudly
// and in the window, and what is in use is still the choice.
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTest>
#include <cstdio>
#include <string>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

void click(QQuickView& view, QQuickItem* item) {
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

bool active(QQuickItem* win, const char* name) {
    QQuickItem* item = named(win, QString::fromLatin1(name));
    return item != nullptr && item->property("active").toBool();
}

bool write_file(const QString& path, const char* bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return true;
}

int test_window() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("cfg/preferences.json"));

    KeyMap map;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    ModelDocument doc(make_empty_session(4, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    session.setPreferencesFile(file);   // no file yet: the defaults
    CHECK(session.prefRefusal().isEmpty());
    CHECK(session.prefCounter() == QStringLiteral("Bars|Beats"));
    CHECK(session.editMode() == QStringLiteral("Slip") &&
          session.gridValue() == QStringLiteral("1/4"));
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    model.tick();

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();

    QQuickItem* win = named(root, QStringLiteral("preferencesWindow"));
    CHECK(win != nullptr && !win->isVisible());
    click(view, named(root, QStringLiteral("setupMenuButton")));
    QQuickItem* item = named(root, QStringLiteral("setupPreferences"));
    CHECK(QTest::qWaitFor([&]() { return item->isVisible(); }));
    click(view, item);
    CHECK(session.preferencesOpen() && win->isVisible());
    // What the engine owns is reported as it is, not offered.
    QQuickItem* audio = named(win, QStringLiteral("prefAudio"));
    CHECK(audio != nullptr);
    CHECK(named(win, QStringLiteral("prefClick")) != nullptr);
    CHECK(named(win, QStringLiteral("prefCountOff")) != nullptr);
    CHECK(named(win, QStringLiteral("prefBuffer")) == nullptr &&
          named(win, QStringLiteral("prefDevice")) == nullptr &&
          named(win, QStringLiteral("prefAutosave")) == nullptr);
    CHECK(named(win, QStringLiteral("pref_counter_Bars|Beats")) != nullptr);
    CHECK(active(win, "pref_counter_Bars|Beats") && active(win, "pref_mode_Slip"));

    // A choice is in use at once and kept.
    click(view, named(win, QStringLiteral("pref_counter_Samples")));
    click(view, named(win, QStringLiteral("pref_mode_Grid")));
    click(view, named(win, QStringLiteral("pref_grid_1/8")));
    CHECK(session.mainCounterFormat() == QStringLiteral("Samples"));
    CHECK(session.editMode() == QStringLiteral("Grid") &&
          session.gridValue() == QStringLiteral("1/8"));
    CHECK(active(win, "pref_mode_Grid") && !active(win, "pref_mode_Slip"));
    CHECK(QFile::exists(file) && session.prefRefusal().isEmpty());

    // The next start finds it.
    {
        ModelDocument doc2(make_empty_session(4, kRate));
        SessionModel next(doc2, peaks, true);
        next.setPreferencesFile(file);
        CHECK(next.prefRefusal().isEmpty());
        CHECK(next.mainCounterFormat() == QStringLiteral("Samples"));
        CHECK(next.editMode() == QStringLiteral("Grid") &&
              next.gridValue() == QStringLiteral("1/8"));
    }

    // A name not in the lists is refused and held; nothing changes, and the
    // next choice clears the notice.
    CHECK(!session.setPreference(QStringLiteral("grid"), QStringLiteral("1/3")));
    CHECK(session.prefRefusal().contains(QStringLiteral("1/3")));
    QQuickItem* refusal = named(win, QStringLiteral("prefRefusal"));
    CHECK(refusal->isVisible() && session.gridValue() == QStringLiteral("1/8"));
    click(view, named(win, QStringLiteral("pref_grid_1/16")));
    CHECK(!refusal->isVisible() && session.gridValue() == QStringLiteral("1/16"));

    // A file this version does not understand: refused, the defaults stay.
    CHECK(write_file(file, "{\"version\": 1, \"grid\": \"1/3\"}"));
    {
        ModelDocument doc3(make_empty_session(4, kRate));
        SessionModel next(doc3, peaks, true);
        next.setPreferencesFile(file);
        CHECK(next.prefRefusal().contains(QStringLiteral("1/3")));
        CHECK(next.gridValue() == QStringLiteral("1/4") &&
              next.prefCounter() == QStringLiteral("Bars|Beats"));
    }
    CHECK(write_file(file, "not json"));
    {
        ModelDocument doc3(make_empty_session(4, kRate));
        SessionModel next(doc3, peaks, true);
        next.setPreferencesFile(file);
        CHECK(!next.prefRefusal().isEmpty());
    }

    // A file that cannot be written: in use now, and the window says it
    // will not be there next time.
    CHECK(write_file(dir.filePath(QStringLiteral("plain")), "x"));
    session.setPreferencesFile(dir.filePath(QStringLiteral("plain/preferences.json")));
    CHECK(!session.setPreference(QStringLiteral("mode"), QStringLiteral("Spot")));
    CHECK(session.editMode() == QStringLiteral("Spot"));
    CHECK(session.prefRefusal().contains(QStringLiteral("not kept")) && refusal->isVisible());

    click(view, named(win, QStringLiteral("preferencesClose")));
    CHECK(!session.preferencesOpen() && !win->isVisible());
    return 0;
}

// Click on, then a restart: it is on after the first state block, and a
// second connect (the engine kept it on) toggles nothing.
int test_click_at_start() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("preferences.json"));
    SyntheticPeakSource peaks(kRate, 120.0);
    {
        ModelDocument doc(make_empty_session(4, kRate));
        SessionModel first(doc, peaks, true);
        first.setPreferencesFile(file);
        CHECK(first.setPreference(QStringLiteral("click"), QStringLiteral("on")));
        CHECK(!first.setPreference(QStringLiteral("click"), QStringLiteral("maybe")));
        CHECK(!first.setPreference(QStringLiteral("buffer"), QStringLiteral("64")));
    }
    ModelDocument doc(make_empty_session(4, kRate));
    SessionModel session(doc, peaks, true);
    QStringList sent;
    session.setCommandSender(
        [&sent](const QString& c, const QString&) { sent << c; });
    session.setPreferencesFile(file);
    CHECK(session.prefClick() && !session.prefCountOff());
    CHECK(sent.isEmpty());   // no engine yet: it waits

    TransportSnapshot t;
    session.onTransport(t, true);   // first state block, click off
    CHECK(sent == QStringList{QStringLiteral("transport.toggle_click")});
    t.click = true;
    session.onTransport(t, true);
    session.onTransport(t, false);   // engine gone
    session.onTransport(t, true);    // second connect, click already on
    CHECK(sent.size() == 1);
    return 0;
}

// Version 2 keeps every choice, the two transport ones included; a session
// that starts from the file finds them all in use.
int test_v2_round_trip() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("preferences.json"));
    SyntheticPeakSource peaks(kRate, 120.0);
    {
        ModelDocument doc(make_empty_session(4, kRate));
        SessionModel session(doc, peaks, true);
        session.setPreferencesFile(file);
        CHECK(session.setPreference(QStringLiteral("counter"), QStringLiteral("Samples")));
        CHECK(session.setPreference(QStringLiteral("grid"), QStringLiteral("1/8")));
        CHECK(session.setPreference(QStringLiteral("mode"), QStringLiteral("Grid")));
        CHECK(session.setPreference(QStringLiteral("click"), QStringLiteral("on")));
        CHECK(session.setPreference(QStringLiteral("countoff"), QStringLiteral("on")));
    }
    QFile f(file);
    CHECK(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    CHECK(o.value(QStringLiteral("version")).toInt() == 2);
    CHECK(o.value(QStringLiteral("counter")).toString() == QStringLiteral("Samples"));
    CHECK(o.value(QStringLiteral("grid")).toString() == QStringLiteral("1/8"));
    CHECK(o.value(QStringLiteral("mode")).toString() == QStringLiteral("Grid"));
    CHECK(o.value(QStringLiteral("click_at_start")).toBool(false));
    CHECK(o.value(QStringLiteral("count_off_at_start")).toBool(false));

    ModelDocument doc(make_empty_session(4, kRate));
    SessionModel next(doc, peaks, true);
    next.setPreferencesFile(file);
    CHECK(next.prefRefusal().isEmpty());
    CHECK(next.mainCounterFormat() == QStringLiteral("Samples"));
    CHECK(next.gridValue() == QStringLiteral("1/8") && next.editMode() == QStringLiteral("Grid"));
    CHECK(next.prefClick() && next.prefCountOff());
    return 0;
}

// A version 1 file predates the transport choices: its missing keys keep the
// defaults, and the file is read rather than refused.
int test_v1_file() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("preferences.json"));
    CHECK(write_file(file, "{\"version\": 1, \"counter\": \"Samples\", "
                           "\"grid\": \"1 bar\", \"mode\": \"Shuffle\"}"));
    SyntheticPeakSource peaks(kRate, 120.0);
    ModelDocument doc(make_empty_session(4, kRate));
    SessionModel session(doc, peaks, true);
    session.setPreferencesFile(file);
    CHECK(session.prefRefusal().isEmpty());
    CHECK(session.mainCounterFormat() == QStringLiteral("Samples"));
    CHECK(session.gridValue() == QStringLiteral("1 bar"));
    CHECK(session.editMode() == QStringLiteral("Shuffle"));
    CHECK(!session.prefClick() && !session.prefCountOff());
    return 0;
}

// A refused choice names what was refused and why, and nothing it refused is
// in use; the next accepted choice clears the notice.
int test_refused_choices() {
    SyntheticPeakSource peaks(kRate, 120.0);
    ModelDocument doc(make_empty_session(4, kRate));
    SessionModel session(doc, peaks, true);
    CHECK(!session.setPreference(QStringLiteral("grid"), QStringLiteral("1/3")));
    CHECK(session.prefRefusal() == QStringLiteral("no preference \"grid\" can be \"1/3\""));
    CHECK(session.gridValue() == QStringLiteral("1/4"));
    CHECK(!session.setPreference(QStringLiteral("click"), QStringLiteral("maybe")));
    CHECK(session.prefRefusal() == QStringLiteral("no preference \"click\" can be \"maybe\""));
    CHECK(!session.prefClick());
    CHECK(!session.setPreference(QStringLiteral("countoff"), QStringLiteral("2")));
    CHECK(session.prefRefusal().contains(QStringLiteral("countoff")));
    CHECK(!session.setPreference(QStringLiteral("buffer"), QStringLiteral("64")));
    CHECK(session.prefRefusal().contains(QStringLiteral("buffer")));
    CHECK(session.setPreference(QStringLiteral("click"), QStringLiteral("on")));
    CHECK(session.prefRefusal().isEmpty() && session.prefClick());
    return 0;
}

// A file this version cannot read is refused with its path and the reason; the
// defaults stay in use.
int test_refused_files() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("preferences.json"));
    SyntheticPeakSource peaks(kRate, 120.0);
    const auto load = [&](const char* bytes) {
        if (!write_file(file, bytes)) return QStringLiteral("<the file was not written>");
        ModelDocument doc(make_empty_session(4, kRate));
        SessionModel session(doc, peaks, true);
        session.setPreferencesFile(file);
        return session.prefRefusal();
    };
    CHECK(load("{\"version\": 2, \"grid\": \"1/3\"}") ==
          file + QStringLiteral(": unknown grid \"1/3\""));
    CHECK(load("{\"version\": 2, \"click_at_start\": \"yes\"}") ==
          file + QStringLiteral(": click_at_start must be true or false"));
    const QString future = load("{\"version\": 3}");
    CHECK(future.contains(QStringLiteral("is version 3")) &&
          future.contains(QStringLiteral("versions 1 to 2")));
    CHECK(load("not json").contains(QStringLiteral("is not a preferences file")));
    ModelDocument doc(make_empty_session(4, kRate));
    SessionModel session(doc, peaks, true);
    CHECK(write_file(file, "{\"version\": 2, \"grid\": \"1/3\", \"click_at_start\": true}"));
    session.setPreferencesFile(file);
    CHECK(!session.prefRefusal().isEmpty() && session.gridValue() == QStringLiteral("1/4"));
    CHECK(!session.prefClick());
    return 0;
}

// The engine owns click and count-off. A choice sends its toggle only when the
// engine's flag differs, and applying the same choice again sends nothing. The
// commands go to the fake engine over ControlAdapter's wire.
int test_apply_once() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    SyntheticPeakSource peaks(kRate, 120.0);
    ModelDocument doc(make_empty_session(4, kRate));
    SessionModel session(doc, peaks, true);
    QStringList sent;
    session.setCommandSender([&sent, &engine](const QString& c, const QString&) {
        sent << c;
        engine.send(c.toStdString(), CommandContext{});
    });
    session.setPreferencesFile(dir.filePath(QStringLiteral("preferences.json")));
    CHECK(sent.isEmpty());   // no engine yet: nothing is sent

    TransportSnapshot t;   // the engine's first block: both flags off
    session.onTransport(t, true);
    CHECK(sent.isEmpty());   // the defaults already match the engine

    const QString toggle_click = QStringLiteral("transport.toggle_click");
    const QString toggle_count_off = QStringLiteral("transport.toggle_count_off");
    CHECK(session.setPreference(QStringLiteral("click"), QStringLiteral("on")));
    CHECK(sent == QStringList{toggle_click});
    CHECK(session.setPreference(QStringLiteral("click"), QStringLiteral("on")));   // applied again
    CHECK(sent.size() == 1);
    CHECK(session.setPreference(QStringLiteral("countoff"), QStringLiteral("off")));   // already off
    CHECK(sent.size() == 1);
    CHECK(session.setPreference(QStringLiteral("countoff"), QStringLiteral("on")));
    CHECK(sent == (QStringList{toggle_click, toggle_count_off}));

    // The engine answers with both on: applying them again sends nothing.
    t.click = true;
    t.count_off = true;
    session.onTransport(t, true);
    CHECK(session.setPreference(QStringLiteral("click"), QStringLiteral("on")));
    CHECK(session.setPreference(QStringLiteral("countoff"), QStringLiteral("on")));
    CHECK(sent.size() == 2);

    CHECK(session.setPreference(QStringLiteral("click"), QStringLiteral("off")));
    CHECK(sent == (QStringList{toggle_click, toggle_count_off, toggle_click}));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_window() || test_click_at_start() || test_v2_round_trip() || test_v1_file() ||
        test_refused_choices() || test_refused_files() || test_apply_once()) {
        return 1;
    }
    std::printf("ui preferences: ok\n");
    return 0;
}
