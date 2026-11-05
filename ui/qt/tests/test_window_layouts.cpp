// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window configurations (ui.workflow.windows): the nine numbered slots of
// open windows and their places, in the model, in the file that keeps them,
// and in the real QML over FakeEngine's wire: Store from the list, Recall
// back, Edit/Mix with it, an empty slot refused and held, a recalled place
// clamped into a smaller display, and the slots surviving a restart.
#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
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
#include "transport_model.h"
#include "window_layouts.h"

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

int test_model() {
    WindowLayouts w;
    QString why;
    // Nothing stored: recalling refuses and changes nothing.
    w.setView(QStringLiteral("mix"));
    CHECK(!w.recall(3, why) && why.contains(QStringLiteral("empty")));
    CHECK(w.current().view == QStringLiteral("mix"));
    CHECK(!w.recall(0, why) && !w.recall(10, why) && !w.store(10, why));
    CHECK(!w.stored(3) && w.summary(3) == QStringLiteral("empty"));

    w.setOpen(QStringLiteral("latency"), true);
    w.place(QStringLiteral("latency"), 40, 50);
    w.place(QStringLiteral("no_such_window"), 1, 2);   // not a window: ignored
    CHECK(w.current().windows.count(QStringLiteral("no_such_window")) == 0);
    CHECK(w.store(2, why));
    CHECK(w.summary(2) == QStringLiteral("Mix, 1 floating"));

    // The screen moves on; recall brings the slot back, closing what it
    // did not have open and keeping a place it never recorded.
    w.setView(QStringLiteral("edit"));
    w.setOpen(QStringLiteral("latency"), false);
    w.setOpen(QStringLiteral("transport"), true);
    w.place(QStringLiteral("latency"), 400, 500);
    w.place(QStringLiteral("transport"), 7, 8);
    CHECK(w.recall(2, why));
    CHECK(w.current().view == QStringLiteral("mix"));
    CHECK(w.current().windows.at(QStringLiteral("latency")).open);
    CHECK(w.current().windows.at(QStringLiteral("latency")).x == 40);
    CHECK(!w.current().windows.at(QStringLiteral("transport")).open);
    CHECK(w.current().windows.at(QStringLiteral("transport")).x == 7);

    // Through the file, and an unknown window written by a newer build is
    // dropped rather than refusing the file.
    WindowLayouts r;
    CHECK(r.fromJson(w.toJson(), why) && r.stored(2) && !r.stored(1));
    CHECK(r.recall(2, why) && r.current().windows.at(QStringLiteral("latency")).y == 50);
    CHECK(r.fromJson(
        R"({"version":1,"configurations":{"1":{"view":"edit","windows":{"future":{"open":true}}}}})",
        why));
    CHECK(r.stored(1) && !r.stored(2));
    CHECK(!r.fromJson("not json", why) && r.stored(1));   // left as it was
    CHECK(!r.fromJson(R"({"version":99})", why));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("cfg/window-layouts.json"));
    CHECK(r.load(file, why));   // none yet is not a fault
    CHECK(w.save(file, why));
    WindowLayouts again;
    CHECK(again.load(file, why) && again.stored(2));
    return 0;
}

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

QString text_of(QQuickItem* root, const char* name) {
    QQuickItem* item = named(root, QString::fromLatin1(name));
    return item != nullptr ? item->property("text").toString() : QStringLiteral("<none>");
}

int test_ui() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("window-layouts.json"));

    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    KeyMap map;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    ModelDocument doc(make_empty_session(4, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    TransportModel transport(
        [&session]() -> const SessionSettings& { return session.controller().snapshot().settings; },
        [&session]() { return session.controller().selection().insertion; });
    transport.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    model.tick();
    link_transport(model, session, transport);
    session.setLayoutsFile(file);
    QSignalSpy held(&session, &SessionModel::heldNotice);

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)},
                               {QStringLiteral("transport"), QVariant::fromValue(&transport)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();
    QQuickItem* history = named(root, QStringLiteral("undoHistoryLoader"));
    QQuickItem* latency = named(root, QStringLiteral("latencyLoader"));
    QQuickItem* list = named(root, QStringLiteral("windowConfigsWindow"));
    CHECK(history != nullptr && latency != nullptr && list != nullptr && !list->isVisible());

    // Setup > Window Configurations… shows the nine slots, all empty.
    click(view, named(root, QStringLiteral("setupMenuButton")));
    QQuickItem* entry = named(root, QStringLiteral("setupWindowConfigs"));
    CHECK(QTest::qWaitFor([&]() { return entry->isVisible(); }));
    click(view, entry);
    CHECK(session.windowConfigsOpen() && list->isVisible());
    CHECK(text_of(list, "windowConfigSummary1") == QStringLiteral("empty"));
    CHECK(text_of(list, "windowConfigSummary9") == QStringLiteral("empty"));

    // Recalling an empty slot moves nothing, and the reason is held, not
    // only on the 2.5 s status line.
    session.setWindow(QStringLiteral("mix"));
    held.clear();
    CHECK(!model.invokeWith(QStringLiteral("window.config_recall"), -1, QStringLiteral("4")));
    CHECK(held.count() == 1 && held.at(0).at(0).toString().contains(QStringLiteral("empty")));
    CHECK(session.window() == QStringLiteral("mix"));
    CHECK(!model.invokeWith(QStringLiteral("window.config_recall"), -1, QStringLiteral("x")));
    CHECK(!model.invokeWith(QStringLiteral("window.config_store"), -1, QStringLiteral("12")));

    // Arrange the screen: Edit in front, Undo History and Transport open,
    // the history window moved. Store it as 1.
    session.setWindow(QStringLiteral("edit"));
    model.invoke(QStringLiteral("window.undo_history"), -1);
    model.invoke(QStringLiteral("window.transport"), -1);
    history->setX(300);
    history->setY(120);
    CHECK(session.undoHistoryOpen() && transport.open());
    click(view, named(list, QStringLiteral("windowConfigStore1")));
    // The list rebinds on the next event-loop pass, which a loaded runner can delay.
    QTest::qWaitFor([&]() { return text_of(list, "windowConfigSummary1") == QStringLiteral("Edit, 2 floating"); }, 2000);
    CHECK(text_of(list, "windowConfigSummary1") == QStringLiteral("Edit, 2 floating"));

    // Arrange another: Mix in front, only Latency open. Store it as 2.
    model.invoke(QStringLiteral("window.undo_history"), -1);
    model.invoke(QStringLiteral("window.transport"), -1);
    model.invoke(QStringLiteral("window.latency"), -1);
    session.setWindow(QStringLiteral("mix"));
    latency->setX(50);
    latency->setY(60);
    click(view, named(list, QStringLiteral("windowConfigStore2")));
    // The list rebinds on the next event-loop pass, which a loaded runner can delay.
    QTest::qWaitFor([&]() { return text_of(list, "windowConfigSummary2") == QStringLiteral("Mix, 1 floating"); }, 2000);
    CHECK(text_of(list, "windowConfigSummary2") == QStringLiteral("Mix, 1 floating"));

    // Recall 1: the view, the open set and the place all come back.
    click(view, named(list, QStringLiteral("windowConfigRecall1")));
    CHECK(session.window() == QStringLiteral("edit"));
    CHECK(session.undoHistoryOpen() && transport.open() && !session.latencyOpen());
    CHECK(history->x() == 300 && history->y() == 120);
    // And 2 back again; the toggle (Ctrl+=) still flips Edit/Mix by itself.
    click(view, named(list, QStringLiteral("windowConfigRecall2")));
    CHECK(session.window() == QStringLiteral("mix"));
    CHECK(!session.undoHistoryOpen() && !transport.open() && session.latencyOpen());
    CHECK(latency->x() == 50 && latency->y() == 60);
    model.invoke(QStringLiteral("window.toggle_mix_edit"), -1);
    CHECK(session.window() == QStringLiteral("edit"));

    // The refusal of an empty slot stays in the list until the next action.
    click(view, named(list, QStringLiteral("windowConfigRecall7")));
    CHECK(text_of(list, "windowConfigRefusal").contains(QStringLiteral("empty")));
    CHECK(session.window() == QStringLiteral("edit"));
    click(view, named(list, QStringLiteral("windowConfigStore7")));
    CHECK(text_of(list, "windowConfigRefusal").isEmpty());

    // A configuration made on a bigger display: the place is moved back
    // inside this one instead of leaving the window off the screen.
    history->setX(1200);
    history->setY(700);
    CHECK(model.invokeWith(QStringLiteral("window.config_store"), -1, QStringLiteral("3")));
    view.resize(800, 500);
    QTest::qWait(50);
    history->setX(10);
    history->setY(10);
    CHECK(model.invokeWith(QStringLiteral("window.config_recall"), -1, QStringLiteral("3")));
    CHECK(history->x() + history->width() <= root->width() + 0.5);
    CHECK(history->y() + history->height() <= root->height() + 0.5);

    // The slots are kept per machine: a new run reads them back.
    held.clear();
    SessionModel next(doc, peaks, true);
    next.setLayoutsFile(file);
    CHECK(next.layouts().stored(1) && next.layouts().stored(2) && next.layouts().stored(3));
    CHECK(held.isEmpty());

    // A session opening puts the list away; the slots stay.
    emit session.sessionOpened();
    CHECK(!session.windowConfigsOpen() && session.layouts().stored(1));

    // A file that cannot be read is said so, held, and the slots stay as they were.
    QFile bad(file);
    CHECK(bad.open(QIODevice::WriteOnly) && bad.write("not json") > 0);
    bad.close();
    QSignalSpy held_next(&next, &SessionModel::heldNotice);
    next.setLayoutsFile(file);
    CHECK(held_next.count() == 1 &&
          held_next.at(0).at(0).toString().contains(QStringLiteral("not loaded")));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_model()) return 1;
    if (test_ui()) return 1;
    std::printf("ui window layouts: ok\n");
    return 0;
}
