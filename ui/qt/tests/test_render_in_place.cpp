// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track > Freeze, Commit, Unfreeze and Process Selection (plugins.graph.freeze,
// plugins.offline; ADR-0051), in the real QML over FakeEngine's wire: the
// Track menu names the selected track (and for Process its selected range)
// in the argument it sends; progress shows in the footer with a Cancel while
// it runs; how it ended is held until dismissed, not on the status line; a
// refusal of the engine's is held the same way; Process without a range is
// said before anything is sent.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/api/render.h"
#include "sonora/model/operations.h"
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

QString text_of(QQuickItem* root, const char* name) {
    QQuickItem* item = named(root, QString::fromLatin1(name));
    return item != nullptr ? item->property("text").toString() : QStringLiteral("<none>");
}

int track_menu_over_the_fake_engine() {
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
    std::string sent_command, sent_argument;
    KeyRouter router(map, engine.info().implemented_commands,
                     [&](const std::string& c, const CommandContext& ctx) {
                         if (c.compare(0, 6, "track.") == 0) {
                             sent_command = c;
                             sent_argument = ctx.argument;
                         }
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    session.setCommandSender([&model](const QString& command, const QString& argument) {
        model.invokeWith(command, -1, argument);
    });
    model.tick();
    CHECK(model.renderAvailable() && !model.renderRunning() && model.renderNotice().isEmpty());
    const int base = model.notices().size();

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

    const auto open_menu = [&]() {
        click(view, named(root, QStringLiteral("trackMenuButton")));
        QQuickItem* item = named(root, QStringLiteral("trackFreeze"));
        return QTest::qWaitFor([&]() { return item->isVisible(); });
    };

    // A selected track: Freeze sends its id; there is nothing to unfreeze.
    session.controller().select_track(1);
    QMetaObject::invokeMethod(&session, "selectionChanged");
    CHECK(session.selectedTrack() == 1 && !session.selectedTrackFrozen());
    CHECK(open_menu());
    CHECK(named(root, QStringLiteral("trackFreeze"))->property("available").toBool());
    CHECK(!named(root, QStringLiteral("trackUnfreeze"))->property("available").toBool());
    // This engine does not announce track.render.insert (ADR-0087).
    CHECK(!model.renderInsertAvailable());
    CHECK(!named(root, QStringLiteral("trackProcessWith"))->property("available").toBool());
    click(view, named(root, QStringLiteral("trackFreeze")));
    api::RenderOptions sent;
    std::string why;
    CHECK(sent_command == "track.freeze");
    CHECK(api::parse_render_argument(sent_argument, sent, why));
    CHECK(sent.track == session.controller().snapshot().edit->tracks[1]->id && sent.end == 0);
    CHECK(session.renderWhat().startsWith(QStringLiteral("Freezing ")));

    // Running: in the footer, with a Cancel; no notice yet.
    model.tick();
    CHECK(model.renderRunning() && model.renderProgress() == 0.5);
    QQuickItem* status = named(root, QStringLiteral("renderStatus"));
    CHECK(status->isVisible());
    CHECK(text_of(root, "renderText").contains(QStringLiteral("50%")));
    CHECK(named(root, QStringLiteral("cancelRender")) != nullptr);
    model.tick();
    CHECK(!model.renderRunning() && !model.renderNotice().isEmpty());

    // The end is held, not the status line, and outlives it.
    CHECK(!model.statusText().contains(model.renderNotice()));
    QTest::qWait(2700);
    model.tick();
    CHECK(status->isVisible() && text_of(root, "renderText") == model.renderNotice());
    CHECK(model.notices().size() == base);
    click(view, named(root, QStringLiteral("dismissRender")));
    CHECK(model.renderNotice().isEmpty() && !status->isVisible());

    // Process Selection needs the range, and says so without sending.
    sent_command.clear();
    CHECK(!session.renderInPlace(QStringLiteral("process")));
    CHECK(sent_command.empty());
    session.controller().select_range(1, 24000, 48000);
    QMetaObject::invokeMethod(&session, "selectionChanged");
    CHECK(session.renderInPlace(QStringLiteral("process")));
    CHECK(sent_command == "track.process_selection");
    CHECK(api::parse_render_argument(sent_argument, sent, why));
    CHECK(sent.start == 24000 && sent.end == 48000);
    model.tick();
    CHECK(model.renderRunning());
    // Cancel ends it with its own held notice.
    model.cancelRender();
    model.tick();
    CHECK(!model.renderRunning() && model.renderNotice().startsWith(QStringLiteral("Render stopped")));
    model.dismissRender();

    // The engine's refusal is held in the footer, not on the 2.5 s line: the
    // transport rolls, and a render needs it stopped.
    CHECK(model.invokeWith(QStringLiteral("transport.play"), -1, QString()) || true);
    model.tick();
    CHECK(session.renderInPlace(QStringLiteral("freeze")));
    model.tick();
    CHECK(model.renderNotice().startsWith(QStringLiteral("Render refused: ")));
    CHECK(model.renderNotice().contains(QStringLiteral("stop the transport")));
    QTest::qWait(2700);
    model.tick();
    CHECK(model.renderNotice().contains(QStringLiteral("stop the transport")));
    CHECK(status->isVisible());
    return 0;
}

// Track > Process Selection With (ADR-0087): an engine announcing
// track.render.insert, a range on a track with three inserts, one bypassed
// and one inactive. The menu offers only the playing one and the pick sends
// insert=<its id>; the whole-chain entry still sends no insert word.
int process_with_one_insert() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    cfg.capabilities.push_back(api::kCapabilityRenderInsert);
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));

    KeyMap map;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    ModelDocument doc(make_empty_session(4, kRate));
    const sonora::model::Id track = doc.snapshot().edit->tracks[1]->id;
    const auto add = [&](std::uint8_t slot, const char* name) {
        sonora::model::AddInsert a;
        a.track = track;
        a.insert = doc.reserve_id();
        a.slot = slot;
        a.plugin.id = std::string("urn:sonora:test:") + name;
        a.plugin.name = name;
        std::string why;
        return doc.submit(a, why) ? a.insert : sonora::model::Id{0};
    };
    const sonora::model::Id limiter = add(0, "Limiter");
    const sonora::model::Id reverb = add(1, "Plate Reverb");
    const sonora::model::Id delay = add(2, "Tape Delay");
    CHECK(limiter != 0 && reverb != 0 && delay != 0);
    std::string why;
    CHECK(doc.submit(sonora::model::SetInsertBypass{limiter, true}, why));
    CHECK(doc.submit(sonora::model::SetInsertActive{delay, false}, why));

    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    std::string sent_command, sent_argument;
    KeyRouter router(map, engine.info().implemented_commands,
                     [&](const std::string& c, const CommandContext& ctx) {
                         if (c.compare(0, 6, "track.") == 0) {
                             sent_command = c;
                             sent_argument = ctx.argument;
                         }
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    session.setCommandSender([&model](const QString& command, const QString& argument) {
        model.invokeWith(command, -1, argument);
    });
    model.tick();
    CHECK(model.renderAvailable() && model.renderInsertAvailable());

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s
", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();
    const auto open_menu = [&]() {
        click(view, named(root, QStringLiteral("trackMenuButton")));
        QQuickItem* item = named(root, QStringLiteral("trackProcessWith"));
        return QTest::qWaitFor([&]() { return item->isVisible(); });
    };

    // No range yet: the entry waits for one.
    session.controller().select_track(1);
    QMetaObject::invokeMethod(&session, "selectionChanged");
    CHECK(!session.rangeOnSelectedTrack());
    CHECK(open_menu());
    CHECK(!named(root, QStringLiteral("trackProcessWith"))->property("available").toBool());
    named(root, QStringLiteral("trackMenu"))->setProperty("visible", false);

    // Only the active, non-bypassed insert is offered, by name.
    session.controller().select_range(1, 24000, 48000);
    QMetaObject::invokeMethod(&session, "selectionChanged");
    CHECK(session.rangeOnSelectedTrack());
    const QVariantList offered = session.selectedTrackInserts();
    CHECK(offered.size() == 1);
    CHECK(offered[0].toMap()[QStringLiteral("name")].toString() == QStringLiteral("Plate Reverb"));
    CHECK(open_menu());
    QQuickItem* with = named(root, QStringLiteral("trackProcessWith"));
    CHECK(with->property("available").toBool());
    click(view, with);
    QQuickItem* sub = named(root, QStringLiteral("processWithMenu"));
    CHECK(QTest::qWaitFor([&]() { return sub->isVisible(); }));
    QQuickItem* pick = named(root, QStringLiteral("processWith0"));
    CHECK(pick != nullptr && pick->property("text").toString() == QStringLiteral("Plate Reverb"));
    CHECK(named(root, QStringLiteral("processWith1")) == nullptr);
    if (const char* shot = std::getenv("SONORA_PROCESS_WITH_SHOT")) {
        CHECK(view.grabWindow().save(QString::fromLocal8Bit(shot)));
    }

    // The pick sends the range and insert=<the reverb's id>, and closes both.
    click(view, pick);
    CHECK(sent_command == "track.process_selection");
    api::RenderOptions sent;
    CHECK(api::parse_render_argument(api::kCommandProcessSelection, sent_argument, sent, why));
    CHECK(sent.track == track && sent.start == 24000 && sent.end == 48000);
    CHECK(sent.only_insert == reverb);
    CHECK(!sub->isVisible() && !named(root, QStringLiteral("trackMenu"))->isVisible());
    model.tick();
    CHECK(model.renderRunning());
    model.cancelRender();
    model.tick();
    model.dismissRender();

    // The model refuses a bypassed insert, and any insert on Freeze, without sending.
    sent_command.clear();
    CHECK(!session.renderInPlace(QStringLiteral("process"), QString::number(limiter)));
    CHECK(!session.renderInPlace(QStringLiteral("freeze"), QString::number(reverb)));
    CHECK(sent_command.empty());

    // The whole chain still sends no insert word.
    CHECK(session.renderInPlace(QStringLiteral("process")));
    CHECK(api::parse_render_argument(api::kCommandProcessSelection, sent_argument, sent, why));
    CHECK(sent.only_insert == 0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (track_menu_over_the_fake_engine()) return 1;
    if (process_with_one_insert()) return 1;
    std::printf("ui render in place: ok\n");
    return 0;
}
