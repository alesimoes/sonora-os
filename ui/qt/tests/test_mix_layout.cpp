// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's layout at the 1920x1080 reference, for 18 tracks (a
// UMC1820's inputs) and for 48 (ui-design.md, "Mixer strip anatomy"):
//
//   - a strip is the Regular width, 96 px, at either count: it is not the
//     window divided by the number of strips (regression: 37 px at 48);
//   - past what fits the strips scroll sideways, and the master, Wide and
//     pinned at the right, does not move when they do;
//   - the fader zone is at least 55 % of the strip's height, with every
//     insert and send slot in use (they scroll inside their own frames
//     before the fader gives up the space), and at least 240 px of throw;
//   - an insert or send frame keeps at least two slots in view, and nothing
//     overlaps the fader zone.
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
    double worst = 1.0;
    double worstThrow = 1e9;
    const auto check_strip = [&](QQuickItem* zone, double& share, double& throw_px) {
        QQuickItem* strip = zone->parentItem();
        share = zone->height() / strip->height();
        throw_px = zone->height() - 26.0;  // the cap's height
        worst = std::min(worst, share);
        worstThrow = std::min(worstThrow, throw_px);
        return share >= 0.55 - 1e-9 && throw_px >= 240.0;
    };
    double share = 0, tp = 0;
    for (int t = 0; t < tracks; ++t) {
        QQuickItem* zone = named(root, QStringLiteral("faderZone%1").arg(t));
        CHECK(zone != nullptr);
        CHECK(check_strip(zone, share, tp));
        QQuickItem* strip = zone->parentItem();
        QQuickItem* frame = named(strip, QStringLiteral("insertFrame%1").arg(t));
        QQuickItem* sendFrame = named(strip, QStringLiteral("sendFrame%1").arg(t));
        CHECK(frame != nullptr && sendFrame != nullptr);
        const double slot = theme::kInsertSlotHeight;
        CHECK(frame->height() >= 2 * slot);
        CHECK(sendFrame->height() >= 2 * slot);
        // Nothing sits over the fader.
        CHECK(frame->mapToItem(strip, QPointF(0, frame->height())).y() <= zone->y());
        CHECK(sendFrame->mapToItem(strip, QPointF(0, sendFrame->height())).y() <= zone->y());
    }
    QQuickItem* aux = named(root, QStringLiteral("auxFaderZone0"));
    CHECK(aux != nullptr);
    CHECK(check_strip(aux, share, tp));
    CHECK(check_strip(masterZone, share, tp));
    std::printf("  %d tracks: fader zone %.1f %% of the strip, throw %.0f px (strip %.0f px)\n",
                tracks, worst * 100.0, worstThrow, first->height());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (layout(18) != 0) return 1;
    if (layout(48) != 0) return 1;
    std::printf("ui mix layout: all checks passed\n");
    return 0;
}
