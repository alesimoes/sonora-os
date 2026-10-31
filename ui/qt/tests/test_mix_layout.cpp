// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's layout at the 1920x1080 reference, for 18 tracks (a
// UMC1820's inputs) and for 48 (ui-design.md, "Mixer strip anatomy"):
//
//   - a strip is the Regular width, 96 px, at either count: it is not the
//     window divided by the number of strips (regression: 37 px at 48);
//   - past what fits the strips scroll sideways, and the master, Wide and
//     pinned at the right, does not move when they do;
//   - the fader zone is at least 55 % of the strip's height with the default
//     views (inserts and sends A-E), 35 % with every bank expanded (the
//     banks scroll inside their frames), and takes the height of what is
//     collapsed; at least 240 px of throw throughout;
//   - the insert frame keeps slots A to E in view, the sends their five
//     rows, and nothing overlaps the fader zone;
//   - another session opening resets the views;
//   - no QML "Binding loop" warning is raised while it runs.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <string>

#include "control_adapter.h"
#include "insert_model.h"
#include "key_router.h"
#include "mix_model.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "send_model.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "theme.h"
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
    api::testing::FakeEngine fake;
    ControlAdapter engine{fake};
    ReplicaDocument doc;
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    ShellModel shell{engine, router};
    InsertModel inserts{doc, session};
    SendModel sends{doc, session};
    MixModel mixer;

    static api::testing::FakeEngine::Config config(int tracks) {
        api::testing::FakeEngine::Config c;
        c.tracks = static_cast<uint32_t>(tracks);
        return c;
    }

    explicit Rig(int tracks)
        : fake(config(tracks)),
          doc([this](const api::ModelRequest& r) { return engine.submitModel(r); },
              make_empty_session(static_cast<uint32_t>(tracks), 48000)),
          mixer(session, MixModel::GainSender(), []() { return 0u; }, true) {
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        session.installCommands(router.local());
        router.prepare({});
    }
    bool connect() {
        std::string error;
        if (!engine.connect(error)) return false;
        settle();
        return doc.synced();
    }
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
};

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

int layout(int tracks) {
    Rig r(tracks);
    CHECK(r.connect());
    CHECK(r.session.trackCount() == tracks);
    r.sends.newBus();
    r.settle();

    QQuickView view;
    // The offscreen screen is smaller than the reference, and a window is
    // clamped to it, so the root item is sized instead: layout is its own.
    view.setResizeMode(QQuickView::SizeViewToRootObject);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&r.shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&r.session)},
                               {QStringLiteral("inserts"), QVariant::fromValue(&r.inserts)},
                               {QStringLiteral("sends"), QVariant::fromValue(&r.sends)},
                               {QStringLiteral("mixer"), QVariant::fromValue(&r.mixer)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    r.settle();
    QTest::qWait(50);
    QQuickItem* root = view.rootObject();
    root->setSize(QSizeF(1920, 1080));
    QTest::qWait(50);

    QQuickItem* flick = named(root, QStringLiteral("mixStrips"));
    CHECK(flick != nullptr);
    const double stripPx = theme::kStripRegular;
    const int total = tracks + 1;  // and the aux return

    // Fixed width: every strip steps by 96 px, at 18 tracks and at 48.
    QQuickItem* first = named(root, QStringLiteral("faderZone0"))->parentItem();
    QQuickItem* second = named(root, QStringLiteral("faderZone1"))->parentItem();
    CHECK(std::fabs((second->x() - first->x()) - stripPx) < 0.5);
    CHECK(std::fabs(first->width() + 2 - stripPx) < 0.5);
    QQuickItem* last = named(root, QStringLiteral("faderZone%1").arg(tracks - 1))->parentItem();
    CHECK(std::fabs(last->x() - (tracks - 1) * stripPx) < 0.5);
    CHECK(std::fabs(flick->property("contentWidth").toDouble() - total * stripPx) < 0.5);

    // They scroll when they do not fit, and only then.
    // The aux return is a nineteenth strip at 18 tracks, and may scroll.
    const bool fits = tracks * stripPx <= flick->width();
    std::printf("  %d tracks: strip %.0f px, content %.0f px in %.0f px: %s\n", tracks,
                first->width() + 2, flick->property("contentWidth").toDouble(), flick->width(),
                fits ? "fits" : "scrolls");
    std::printf("  view %d, root %.0f\n", (int)root->width(), root->width());
    CHECK(fits == (tracks == 18));

    // The master is Wide, at the right edge, and stays when the strips move.
    QQuickItem* masterZone = named(root, QStringLiteral("busFaderZone"));
    CHECK(masterZone != nullptr);
    QQuickItem* master = masterZone->parentItem();
    CHECK(std::fabs(master->width() - theme::kStripWide) < 0.5);
    const QPointF before = master->mapToScene(QPointF(0, 0));
    flick->setProperty("contentX", flick->property("contentWidth").toDouble());
    QTest::qWait(20);
    CHECK(master->mapToScene(QPointF(0, 0)) == before);
    CHECK(before.x() + master->width() > root->width() - 24);
    if (!fits) CHECK(flick->property("contentX").toDouble() > 0.0);
    flick->setProperty("contentX", 0.0);

    // Height budgets, on every strip and the aux return and the master.
    // The budgets are the spec's: the default views (inserts and sends A-E)
    // keep the fader zone at 55 % or more; with every view expanded 35 % and
    // 240 px of throw, the banks scrolling inside their frames; collapsed
    // views give their height to the fader.
    struct Result {
        double worst = 1.0;
        double worstThrow = 1e9;
    };
    const auto budgets = [&](double minShare, bool frames, Result& out) {
        out = Result{};
        const auto check_strip = [&](QQuickItem* zone) {
            QQuickItem* strip = zone->parentItem();
            const double share = zone->height() / strip->height();
            const double throw_px = zone->height() - 26.0;  // the cap's height
            out.worst = std::min(out.worst, share);
            out.worstThrow = std::min(out.worstThrow, throw_px);
            return share >= minShare - 1e-9 && throw_px >= 240.0;
        };
        for (int t = 0; t < tracks; ++t) {
            QQuickItem* zone = named(root, QStringLiteral("faderZone%1").arg(t));
            if (zone == nullptr || !check_strip(zone)) return false;
            if (!frames) continue;
            QQuickItem* strip = zone->parentItem();
            QQuickItem* frame = named(strip, QStringLiteral("insertFrame%1").arg(t));
            QQuickItem* sendFrame = named(strip, QStringLiteral("sendFrame%1").arg(t));
            if (frame == nullptr || sendFrame == nullptr) return false;
            const double slot = theme::kInsertSlotHeight;
            if (frame->height() < theme::kStripMinSlots * slot) return false;
            if (sendFrame->height() < 5 * slot) return false;
            // Nothing sits over the fader.
            if (frame->mapToItem(strip, QPointF(0, frame->height())).y() > zone->y()) return false;
            if (sendFrame->mapToItem(strip, QPointF(0, sendFrame->height())).y() > zone->y())
                return false;
        }
        QQuickItem* aux = named(root, QStringLiteral("auxFaderZone0"));
        return aux != nullptr && check_strip(aux) && check_strip(masterZone);
    };
    const auto settle_views = [&]() {
        QTest::qWait(30);
        r.settle();
    };
    Result def, expanded, collapsed;

    // Defaults: inserts A-E, sends A-E, I/O.
    CHECK(r.mixer.showIO() && r.mixer.showInsertsAE() && r.mixer.showSends());
    CHECK(!r.mixer.showInsertsFJ() && r.sends.bank() == 0);
    CHECK(budgets(0.55, true, def));

    // Everything expanded: F-J of inserts and sends too.
    r.mixer.setShowInsertsFJ(true);
    r.sends.setBank(5);
    settle_views();
    CHECK(budgets(0.35, true, expanded));
    CHECK(expanded.worst < def.worst);
    CHECK(named(root, QStringLiteral("insertFrame0"))->height() > 5 * theme::kInsertSlotHeight);

    // Collapsed: no inserts, no sends, no I/O. Their height is the fader's.
    r.mixer.setShowInsertsAE(false);
    r.mixer.setShowInsertsFJ(false);
    r.mixer.setShowSends(false);
    r.mixer.setShowIO(false);
    r.sends.setBank(0);
    settle_views();
    CHECK(budgets(0.55, false, collapsed));
    CHECK(collapsed.worst > def.worst);
    CHECK(!named(root, QStringLiteral("insertFrame0"))->isVisible());
    CHECK(!named(root, QStringLiteral("output0"))->isVisible());
    std::printf("  %d tracks: fader zone %.1f %% default, %.1f %% expanded, %.1f %% collapsed, "
                "throw %.0f / %.0f / %.0f px (strip %.0f px)\n",
                tracks, def.worst * 100.0, expanded.worst * 100.0, collapsed.worst * 100.0,
                def.worstThrow, expanded.worstThrow, collapsed.worstThrow, first->height());

    // Inserts A-E shown again bring their frame back.
    r.mixer.setShowInsertsAE(true);
    settle_views();
    CHECK(named(root, QStringLiteral("insertFrame0"))->isVisible());

    // Another session opening puts the views back to their defaults.
    r.mixer.setShowIO(false);
    r.mixer.setShowInsertsFJ(true);
    r.sends.setBank(5);
    emit r.session.sessionOpened();
    CHECK(r.mixer.showIO() && r.mixer.showInsertsAE() && r.mixer.showSends());
    CHECK(!r.mixer.showInsertsFJ() && r.sends.bank() == 0);
    return 0;
}

int g_binding_loops = 0;

void count_binding_loops(QtMsgType, const QMessageLogContext&, const QString& msg) {
    if (msg.contains(QStringLiteral("Binding loop"))) ++g_binding_loops;
    std::fprintf(stderr, "%s\n", qPrintable(msg));
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    qInstallMessageHandler(count_binding_loops);
    if (layout(18) != 0) return 1;
    if (layout(48) != 0) return 1;
    // QML warns, and carries on with a binding that never settles.
    if (g_binding_loops != 0) {
        std::fprintf(stderr, "FAIL: %d \"Binding loop\" warnings\n", g_binding_loops);
        return 1;
    }
    std::printf("ui mix layout: all checks passed\n");
    return 0;
}
