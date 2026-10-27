// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's sends and aux buses (ADR-0022) on FakeEngine, wired as
// main() wires them: ControlAdapter, ReplicaDocument, SessionModel, the
// shell's tick, SendModel. Every check goes through the engine's answer:
//
//   - an empty slot opens the bus chooser; "New bus" makes the bus and the
//     send in one undo step; a new send is -inf, post-fader, FMP on;
//   - the send window's level, pan (FMP off when dragged), PRE and mute are
//     mix state, never an undo step, and a typed level that is not one is
//     refused on the status line;
//   - reassigning a send keeps its values, one undo step;
//   - a bus that would close a loop is offered with the reason, and the
//     engine refuses it if chosen;
//   - removing a bus takes its sends with it, and one undo brings all back;
//   - the QML: send slots on the strips, the aux strip, the bank switch,
//     the chooser and the send window.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "send_model.h"
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

struct Rig {
    api::testing::FakeEngine fake{config()};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    ShellModel shell{engine, router};
    SendModel sends{doc, session};
    std::vector<std::string> refused;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        return c;
    }

    Rig() {
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        doc.on_refused = [this](const std::string& why) { refused.push_back(why); };
        QObject::connect(&session, &SessionModel::refused,
                         [this](const QString& why) { refused.push_back(why.toStdString()); });
        session.installCommands(router.local());
        router.prepare({});
    }

    bool connect() {
        std::string error;
        if (!engine.connect(error)) {
            std::fprintf(stderr, "connect: %s\n", error.c_str());
            return false;
        }
        settle();
        return doc.synced();
    }
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
    SendController& ctl() { return sends.controller(); }
    // The engine's own document: what every check is held to.
    const model::Snapshot& engine_doc() { return fake.document(); }
};

bool said(const std::vector<std::string>& lines, const char* what) {
    for (const std::string& l : lines) {
        if (l.find(what) != std::string::npos) return true;
    }
    std::fprintf(stderr, "  nothing said \"%s\"; said:\n", what);
    for (const std::string& l : lines) std::fprintf(stderr, "    %s\n", l.c_str());
    return false;
}

int new_bus_send_and_values() {
    Rig r;
    CHECK(r.connect());
    // An empty slot: the chooser, on that slot, with no bus yet.
    r.sends.slotClicked(0, 0);
    CHECK(r.sends.chooserOpen() && !r.sends.windowOpen());
    CHECK(r.sends.chooserItems().isEmpty());
    CHECK(r.sends.chooserTitle().contains(QStringLiteral("send A")));
    CHECK(r.sends.choose(-1));
    CHECK(!r.sends.chooserOpen());
    r.settle();
    CHECK(r.engine_doc().edit->buses.size() == 1);
    CHECK(r.engine_doc().edit->buses[0].name == "Bus 1");
    SendSlotView v = r.ctl().slot(0, 0);
    CHECK(v.present && v.bus_name == "Bus 1");
    CHECK(v.level_db == model::kMinGainDb && !v.pre && v.follow_pan && !v.mute);
    CHECK(r.sends.strips().size() == 5);   // four tracks, then the bus
    CHECK(r.sends.auxes().size() == 1);
    // The bus and its send: one undo step.
    CHECK(r.doc.undo_label() == "New Bus Send");

    // The send window: level, pan, PRE, mute.
    r.sends.slotClicked(0, 0);
    CHECK(r.sends.windowOpen() && r.sends.windowStrip() == 0 && r.sends.windowSlot() == 0);
    CHECK(r.sends.window()[QStringLiteral("levelText")].toString() == QStringLiteral("-inf"));
    CHECK(r.sends.setLevelText(QStringLiteral("-6")));
    r.settle();
    CHECK(std::fabs(r.ctl().slot(0, 0).level_db - (-6.0f)) < 1e-4f);
    CHECK(r.sends.window()[QStringLiteral("levelText")].toString() == QStringLiteral("-6.0"));
    CHECK(!r.sends.setLevelText(QStringLiteral("loud")));
    CHECK(!r.refused.empty());
    r.refused.clear();
    r.sends.levelDrag(-20.0, 200.0, false);
    r.settle();
    CHECK(r.ctl().slot(0, 0).level_db > -6.0f);
    r.sends.togglePre();
    r.sends.toggleMute();
    r.sends.panDrag(50.0, 100.0, false);   // half the width: pan +1, FMP off
    r.settle();
    v = r.ctl().slot(0, 0);
    CHECK(v.pre && v.mute && !v.follow_pan && v.pan == 1.0f);
    const model::SendMix* m = r.engine_doc().mix.find_send(v.send);
    CHECK(m != nullptr && m->pre && m->mute && !m->follow_pan);
    // None of that is an undo step.
    CHECK(r.doc.undo_label() == "New Bus Send");
    std::string why;
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(!r.ctl().slot(0, 0).present && r.engine_doc().edit->buses.empty());
    // The window had nothing left to show, and closed.
    CHECK(!r.sends.windowOpen());
    CHECK(r.session.controller().redo(why));
    r.settle();
    v = r.ctl().slot(0, 0);
    CHECK(v.present && v.pre && v.mute);   // values outlive the undo
    CHECK(r.refused.empty());
    std::printf("  new bus send, values, undo: ok\n");
    return 0;
}

int reassign_loop_and_remove_bus() {
    Rig r;
    CHECK(r.connect());
    r.sends.newBus();
    r.sends.newBus();
    r.settle();
    CHECK(r.ctl().bus_count() == 2);
    // Track 1's send A to Bus 1 at -3 dB, then sent to Bus 2: values kept.
    r.sends.openChooser(1, 0);
    CHECK(r.sends.choose(0));
    r.settle();
    r.sends.openWindow(1, 0);
    CHECK(r.sends.setLevelText(QStringLiteral("-3")));
    r.settle();
    r.sends.reassign();
    CHECK(r.sends.chooserOpen());
    CHECK(r.sends.choose(1));
    r.settle();
    SendSlotView v = r.ctl().slot(1, 0);
    CHECK(v.present && v.bus_name == "Bus 2" && std::fabs(v.level_db + 3.0f) < 1e-4f);
    CHECK(r.doc.undo_label() == "Assign Send");
    // The window follows its slot to the new send.
    CHECK(r.sends.windowOpen() && r.sends.window()[QStringLiteral("bus")].toString() == QStringLiteral("Bus 2"));

    // Bus 1 sends to Bus 2; Bus 2's chooser shows Bus 1 with the reason.
    const int bus1 = r.ctl().track_count();
    const int bus2 = bus1 + 1;
    r.sends.openChooser(bus1, 0);
    CHECK(r.sends.choose(1));
    r.settle();
    r.sends.openChooser(bus2, 0);
    const QVariantList items = r.sends.chooserItems();
    CHECK(items.size() == 2);
    CHECK(items[0].toMap()[QStringLiteral("refusal")].toString().contains(QStringLiteral("feedback loop")));
    CHECK(items[1].toMap()[QStringLiteral("refusal")].toString().contains(QStringLiteral("itself")));
    // Chosen anyway, the engine refuses it and says why.
    CHECK(!r.sends.choose(0) || (r.settle(), true));
    r.settle();
    CHECK(said(r.refused, "feedback loop"));
    CHECK(!r.ctl().slot(bus2, 0).present);
    r.sends.closeChooser();
    r.refused.clear();

    // Removing Bus 2 takes the two sends that feed it, one undo step.
    r.sends.removeBus(1);
    r.settle();
    CHECK(r.ctl().bus_count() == 1);
    CHECK(!r.ctl().slot(1, 0).present && !r.ctl().slot(bus1, 0).present);
    CHECK(r.doc.undo_label() == "Remove Bus");
    std::string why;
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(r.ctl().bus_count() == 2 && r.ctl().slot(1, 0).present && r.ctl().slot(bus1, 0).present);

    // The aux strip's own values.
    r.sends.toggleAuxMute(0);
    CHECK(r.sends.setAuxGainText(0, QStringLiteral("-10")));
    r.settle();
    const AuxView a = r.ctl().aux(0);
    CHECK(a.mute && std::fabs(a.gain_db + 10.0f) < 1e-4f);
    CHECK(r.engine_doc().mix.find_bus(a.id)->mute);
    CHECK(r.refused.empty());
    std::printf("  reassign, loop refused, bus removed: ok\n");
    return 0;
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QPoint centre(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

int qml() {
    Rig r;
    CHECK(r.connect());
    QQuickView view;
    view.resize(1280, 800);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&r.shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&r.session)},
                               {QStringLiteral("sends"), QVariant::fromValue(&r.sends)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    r.shell.routeKeysFrom(&view);
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();

    // "+ Bus" adds an aux strip after the tracks.
    QQuickItem* add = named(root, QStringLiteral("newBus"));
    CHECK(add != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(add));
    r.settle();
    QTest::qWait(30);
    CHECK(r.ctl().bus_count() == 1);
    CHECK(named(root, QStringLiteral("aux0")) != nullptr);

    // A click on track 0's empty send A: the chooser; a click on the bus
    // makes the send, shown on the slot.
    QQuickItem* slot = named(root, QStringLiteral("send_0_0"));
    CHECK(slot != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(slot));
    QTest::qWait(30);
    QQuickItem* chooser = named(root, QStringLiteral("sendChooser"));
    CHECK(chooser != nullptr && chooser->isVisible());
    QQuickItem* choice = named(root, QStringLiteral("sendChoice0"));
    CHECK(choice != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(choice));
    r.settle();
    QTest::qWait(30);
    CHECK(!chooser->isVisible());
    CHECK(r.ctl().slot(0, 0).present);

    // A click on it now: the send window; PRE there makes it pre-fader.
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(slot));
    QTest::qWait(30);
    QQuickItem* window = named(root, QStringLiteral("sendWindow"));
    CHECK(window != nullptr && window->isVisible());
    QQuickItem* pre = named(root, QStringLiteral("sendWindowPre"));
    CHECK(pre != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(pre));
    r.settle();
    QTest::qWait(30);
    CHECK(r.ctl().slot(0, 0).pre && pre->property("active").toBool());

    // The bank switch shows F-J: slot 0 on the strip is now send F.
    QQuickItem* fj = named(root, QStringLiteral("sendBankFJ"));
    CHECK(fj != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(fj));
    QTest::qWait(30);
    CHECK(r.sends.bank() == 5);
    CHECK(named(root, QStringLiteral("send_0_5")) != nullptr);
    CHECK(named(root, QStringLiteral("send_0_0")) == nullptr);
    CHECK(r.refused.empty());
    std::printf("  qml: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (new_bus_send_and_values() != 0) return 1;
    if (reassign_loop_and_remove_bus() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui sends: all checks passed\n");
    return 0;
}
