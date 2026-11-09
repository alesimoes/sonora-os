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
//   - another session opening resets the views, an Open of a session file
//     and of an unsaved recording alike, and a refused Open does not;
//   - at 1280x720 the header's controls sit apart, and the elided note
//     ends before the first of them;
//   - strips hidden from the Mix window (per strip, per kind) leave the
//     models alone, close the cards up, never include the master, are not
//     saved, and are cleared when another session opens;
//   - no QML "Binding loop" warning is raised while it runs.
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
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
    QTemporaryDir dir;

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
        session.setSessionsDir(dir.path());
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
    bool save(std::string& why) {
        // Save asks the engine for the plugins' state and writes on its reply.
        if (!session.saveSession(why)) return false;
        settle();
        return !session.saveWaiting();
    }
};

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

// The Mix window of `r`, its root item sized `w` x `h`; null if it did not
// load.
QQuickItem* show_mix(Rig& r, QQuickView& view, int w, int h) {
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
    if (view.status() != QQuickView::Ready) return nullptr;
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    if (!QTest::qWaitForWindowExposed(&view)) return nullptr;
    r.settle();
    QTest::qWait(50);
    QQuickItem* root = view.rootObject();
    root->setSize(QSizeF(w, h));
    QTest::qWait(50);
    return root;
}

int layout(int tracks) {
    Rig r(tracks);
    CHECK(r.connect());
    CHECK(r.session.trackCount() == tracks);
    r.sends.newBus();
    r.settle();
    std::string why;
    CHECK(r.save(why));
    const QString saved = r.session.sessionFile();
    CHECK(!saved.isEmpty());

    QQuickView view;
    QQuickItem* root = show_mix(r, view, 1920, 1080);
    CHECK(root != nullptr);

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

    // The two F-J buttons side by side in the header are told apart by name.
    {
        QQuickItem* ins = named(root, QStringLiteral("viewInsertsFJ"));
        QQuickItem* snd = named(root, QStringLiteral("sendBankFJ"));
        CHECK(ins != nullptr && snd != nullptr);
        CHECK(ins->property("text").toString() == QStringLiteral("Ins F-J"));
        CHECK(snd->property("text").toString() == QStringLiteral("Sends F-J"));
    }

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

    // Another session opening puts the views back to their defaults; an Open
    // refused before anything is replaced leaves them as they are.
    QSignalSpy opened(&r.session, &SessionModel::sessionOpened);
    const auto change_views = [&]() {
        r.mixer.setShowIO(false);
        r.mixer.setShowInsertsFJ(true);
        r.sends.setBank(5);
    };
    const auto views_changed = [&]() {
        return !r.mixer.showIO() && r.mixer.showInsertsFJ() && r.sends.bank() == 5;
    };
    const auto views_reset = [&]() {
        return r.mixer.showIO() && r.mixer.showInsertsAE() && r.mixer.showSends() &&
               !r.mixer.showInsertsFJ() && r.sends.bank() == 0;
    };
    change_views();
    CHECK(!r.session.openSessionFile(QDir(r.dir.path()).filePath(QStringLiteral("None.sonora")),
                                     true, why));
    r.settle();
    CHECK(opened.isEmpty() && views_changed());
    CHECK(r.session.openSessionFile(saved, true, why));
    r.settle();
    CHECK(opened.size() == 1 && views_reset());

    // The recovery path's Open, of an unsaved recording, is another session.
    change_views();
    const QDir sessions(r.dir.path());
    CHECK(sessions.mkpath(QStringLiteral("Untitled 2/Audio Files")));
    CHECK(r.session.openRecording(sessions.filePath(QStringLiteral("Untitled 2")), true));
    r.settle();
    CHECK(opened.size() == 2 && views_reset());
    return 0;
}

// Strips the Mix window hides (ADR-0130): view state of the window, not of
// the session. Hidden strips stay in the models; the cards after them close up.
int strip_visibility() {
    Rig r(6);
    CHECK(r.connect());
    r.sends.newBus();
    r.settle();
    std::string why;
    CHECK(r.save(why));
    const QString saved = r.session.sessionFile();
    QQuickView view;
    QQuickItem* root = show_mix(r, view, 1920, 1080);
    CHECK(root != nullptr);
    QQuickItem* button = named(root, QStringLiteral("stripsButton"));
    CHECK(button != nullptr);
    QQuickItem* mix = button;
    while (mix != nullptr && !mix->property("hiddenIds").isValid()) mix = mix->parentItem();
    CHECK(mix != nullptr);
    const double px = theme::kStripRegular;
    const auto click = [&](const QString& name) {
        QQuickItem* i = named(root, name);
        if (i == nullptr) return false;
        const bool ok = QMetaObject::invokeMethod(i, "clicked", Q_ARG(QVariant, QVariant()));
        QTest::qWait(20);
        return ok;
    };
    const auto call = [&](const char* fn, const QString& key, bool hide) {
        QMetaObject::invokeMethod(mix, fn, Q_ARG(QVariant, key), Q_ARG(QVariant, hide));
        QTest::qWait(20);
    };
    const auto count = [&]() { return mix->property("hiddenCount").toInt(); };
    const auto strip = [&](int t) {
        return named(root, QStringLiteral("faderZone%1").arg(t))->parentItem();
    };
    const auto aux = [&]() { return named(root, QStringLiteral("auxFaderZone0"))->parentItem(); };
    QQuickItem* badge = named(root, QStringLiteral("hiddenBadge"));
    QQuickItem* master = named(root, QStringLiteral("busFaderZone"))->parentItem();
    CHECK(badge != nullptr && !badge->isVisible() && count() == 0);

    // Hiding strip 1: its card goes, the ones after it close up, the badge counts.
    CHECK(click(QStringLiteral("stripsRow_t1")));
    CHECK(count() == 1 && badge->isVisible());
    CHECK(!strip(1)->isVisible() && strip(0)->isVisible() && strip(2)->isVisible());
    CHECK(std::fabs(strip(0)->x()) < 0.5 && std::fabs(strip(2)->x() - px) < 0.5);
    CHECK(std::fabs(aux()->x() - 5 * px) < 0.5);
    CHECK(std::fabs(named(root, QStringLiteral("mixStrips"))->property("contentWidth").toDouble() -
                    6 * px) < 0.5);
    // It stays in the model: slot, meter and solo are as before.
    CHECK(r.mixer.slotCount() == 6 && r.mixer.trackSlots().value(1) == 1);
    QQuickItem* meter = named(root, QStringLiteral("mixMeter1"));
    CHECK(meter != nullptr && meter->property("channel").toInt() == 1 &&
          meter->property("source").value<QObject*>() == &r.shell);
    CHECK(r.mixer.soloClick(1, 0));
    r.settle();
    CHECK(r.session.solos().value(1));
    CHECK(r.mixer.soloClick(1, 0));

    // The master is pinned.
    call("setStripHidden", QStringLiteral("master"), true);
    call("setStripHidden", QStringLiteral("m0"), true);
    CHECK(count() == 1 && master->isVisible());

    // Per kind, then Show all.
    CHECK(click(QStringLiteral("stripsKind_t")));
    CHECK(count() == 6 && master->isVisible() && aux()->isVisible());
    for (int t = 0; t < 6; ++t) CHECK(!strip(t)->isVisible());
    CHECK(std::fabs(aux()->x()) < 0.5);
    CHECK(click(QStringLiteral("stripsKind_t")));
    CHECK(count() == 0 && strip(5)->isVisible());
    call("setStripHidden", QStringLiteral("t0"), true);
    call("setStripHidden", QStringLiteral("a0"), true);
    CHECK(count() == 2 && !aux()->isVisible());
    CHECK(click(QStringLiteral("stripsShowAll")));
    CHECK(count() == 0 && aux()->isVisible() && strip(0)->isVisible());

    // The popup lists every strip, so a hidden one cannot be lost.
    QQuickItem* popup = named(root, QStringLiteral("stripsPopup"));
    CHECK(popup != nullptr && !popup->isVisible());
    CHECK(QMetaObject::invokeMethod(popup, "toggle"));
    QTest::qWait(20);
    CHECK(popup->isVisible());
    CHECK(named(root, QStringLiteral("stripsRow_t5")) != nullptr &&
          named(root, QStringLiteral("stripsRow_a0")) != nullptr);

    // Not in the saved file.
    call("setStripHidden", QStringLiteral("t3"), true);
    CHECK(r.save(why));
    QFile f(r.session.sessionFile());
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    CHECK(!bytes.isEmpty() && !bytes.contains("hiddenIds") && !bytes.contains("stripsHidden"));

    // Another session opening, a file or an unsaved recording, shows them all.
    QSignalSpy opened(&r.session, &SessionModel::sessionOpened);
    call("setStripHidden", QStringLiteral("t2"), true);
    CHECK(count() == 2);
    CHECK(r.session.openSessionFile(saved, true, why));
    r.settle();
    CHECK(opened.size() == 1 && count() == 0 && strip(2)->isVisible());
    call("setStripHidden", QStringLiteral("t2"), true);
    const QDir sessions(r.dir.path());
    CHECK(sessions.mkpath(QStringLiteral("Untitled 2/Audio Files")));
    CHECK(r.session.openRecording(sessions.filePath(QStringLiteral("Untitled 2")), true));
    r.settle();
    CHECK(opened.size() == 2 && count() == 0);
    return 0;
}

// At 1280x720 the header holds the note, the views, the sends' switch and
// the delay compensation on one line: each control apart from the next, and
// the note, elided, ending before the views begin (PR #182 review).
int header_at_1280() {
    Rig r(18);
    CHECK(r.connect());
    QQuickView view;
    QQuickItem* root = show_mix(r, view, 1280, 720);
    CHECK(root != nullptr);
    const auto left = [](QQuickItem* i) { return i->mapToScene(QPointF(0, 0)).x(); };
    const auto right = [](QQuickItem* i) { return i->mapToScene(QPointF(i->width(), 0)).x(); };
    QQuickItem* note = named(root, QStringLiteral("mixNote"));
    QQuickItem* views = named(root, QStringLiteral("viewControls"));
    QQuickItem* sendsRow = named(root, QStringLiteral("sendControls"));
    QQuickItem* comp = named(root, QStringLiteral("compensation"));
    CHECK(note != nullptr && views != nullptr && sendsRow != nullptr && comp != nullptr);
    CHECK(note->isVisible() && views->isVisible() && sendsRow->isVisible() && comp->isVisible());
    std::printf("  1280: note %.0f-%.0f (text %.0f px), views %.0f-%.0f, sends %.0f-%.0f, "
                "delay %.0f-%.0f\n",
                left(note), right(note), note->implicitWidth(), left(views), right(views),
                left(sendsRow), right(sendsRow), left(comp), right(comp));
    CHECK(note->width() > 0.0);
    QQuickItem* group = named(root, QStringLiteral("groupControls"));
    CHECK(group != nullptr && right(note) <= left(group) && right(group) <= left(views));
    CHECK(right(note) <= left(views));
    CHECK(right(views) <= left(sendsRow));
    CHECK(right(sendsRow) <= left(comp));
    CHECK(right(comp) <= root->width());
    // Longer than its room, the note is cut short, not drawn under the
    // buttons.
    CHECK(note->implicitWidth() <= note->width() || note->property("truncated").toBool());
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
    if (header_at_1280() != 0) return 1;
    if (strip_visibility() != 0) return 1;
    // QML warns, and carries on with a binding that never settles.
    if (g_binding_loops != 0) {
        std::fprintf(stderr, "FAIL: %d \"Binding loop\" warnings\n", g_binding_loops);
        return 1;
    }
    std::printf("ui mix layout: all checks passed\n");
    return 0;
}
