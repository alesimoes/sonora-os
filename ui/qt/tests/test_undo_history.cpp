// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Undo History window (Pro Tools' Window > Undo History): every step
// undo can reach, oldest first, the undone ones marked; a click on a done
// step undoes it and every later one, a click on an undone step redoes up to
// it; Undo All and Redo All; a history that has dropped its oldest steps says
// so. Then the window in the real QML, opened from the Edit menu, on the
// offscreen platform.
#include <QFont>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <algorithm>
#include <cstdio>
#include <string>

#include "edit_controller.h"
#include "key_router.h"
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
constexpr Frames kBar = 96000;

// One region on track 1 over a 40-bar source, so Duplicate has something.
Snapshot one_region() {
    Snapshot s = make_empty_session(2, kRate);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 40 * kBar;
    pool->sources.push_back(take);
    s.sources = pool;
    s.next_id = 1000;
    Region r;
    r.id = 10;
    r.name = "A";
    r.position = kBar;
    r.length = kBar;
    r.source_offset = kBar;
    r.channels = {{100, 0}};
    auto edit = std::make_shared<EditState>(*s.edit);
    auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto p = std::make_shared<Playlist>(*t->playlists[0]);
    p->regions = {r};
    t->playlists[0] = p;
    edit->tracks[0] = t;
    s.edit = edit;
    return s;
}

std::size_t markers(const TimelineDocument& d) { return d.snapshot().edit->markers->markers.size(); }

std::size_t regions(const TimelineDocument& d) {
    return active_playlist(d.snapshot().edit->tracks[0].get())->regions.size();
}

bool add_marker(SessionModel& session, TimelineDocument& doc, const char* name) {
    std::string why;
    const bool ok = doc.submit(AddMarker{Marker{doc.reserve_id(), 4800, name, 0}}, why);
    session.documentUpdated();
    return ok;
}

QString label(const QVariantList& rows, int i) {
    return rows[i].toMap()[QStringLiteral("label")].toString();
}
bool done(const QVariantList& rows, int i) { return rows[i].toMap()[QStringLiteral("done")].toBool(); }

// Duplicate, then two markers: three steps after the session as opened.
int test_rows_and_jumps() {
    ModelDocument doc(one_region());
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CHECK(session.undoHistory().isEmpty() && !session.undoHistoryTrimmed());

    std::string why;
    CHECK(session.controller().select_region(10));
    CHECK(session.controller().duplicate(why));
    session.documentUpdated();
    CHECK(add_marker(session, doc, "Verse"));
    CHECK(add_marker(session, doc, "Chorus"));

    QVariantList rows = session.undoHistory();
    CHECK(rows.size() == 3);
    CHECK(label(rows, 0) == QStringLiteral("Duplicate"));
    CHECK(label(rows, 1) == QStringLiteral("New Marker"));
    CHECK(done(rows, 0) && done(rows, 1) && done(rows, 2));

    // A click on step 2, done: it and step 3 are undone; the duplicate stays.
    CHECK(session.undoHistoryClick(1));
    CHECK(doc.history().undo_depth() == 1);
    CHECK(markers(doc) == 0 && regions(doc) == 2);
    rows = session.undoHistory();
    CHECK(rows.size() == 3 && done(rows, 0) && !done(rows, 1) && !done(rows, 2));
    CHECK(session.redoText() == QStringLiteral("Redo New Marker"));

    // A click on step 3, undone: redone up to and including it.
    CHECK(session.undoHistoryClick(2));
    CHECK(doc.history().undo_depth() == 3 && markers(doc) == 2);

    // Clicking the newest done step undoes that one only.
    CHECK(session.undoHistoryClick(2));
    CHECK(doc.history().undo_depth() == 2 && markers(doc) == 1);

    CHECK(session.undoHistoryAll(true));
    CHECK(doc.history().undo_depth() == 0 && markers(doc) == 0 && regions(doc) == 1);
    CHECK(!session.undoHistoryAll(true));   // nothing left, and it says so
    CHECK(session.undoHistoryAll(false));
    CHECK(doc.history().undo_depth() == 3 && markers(doc) == 2 && regions(doc) == 2);
    CHECK(!session.undoHistoryAll(false));

    // A row that is not there is refused, and nothing moves.
    CHECK(!session.undoHistoryClick(3) && !session.undoHistoryClick(-1));
    CHECK(doc.history().undo_depth() == 3);
    CHECK(!session.controller().undo_to(4, why) && !why.empty());

    // A new edit after an undo discards what redo would have brought back.
    CHECK(session.undoHistoryClick(1));
    CHECK(add_marker(session, doc, "Bridge"));
    rows = session.undoHistory();
    CHECK(rows.size() == 2 && done(rows, 1));
    return 0;
}

// A history bounded so tight it keeps two steps: the oldest are dropped,
// and the window must not let step 1 pass for the session as opened.
int test_trimmed() {
    ModelDocument doc(one_region(), 1);
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CHECK(add_marker(session, doc, "One"));
    CHECK(!session.undoHistoryTrimmed());
    CHECK(add_marker(session, doc, "Two"));
    CHECK(add_marker(session, doc, "Three"));
    CHECK(session.undoHistoryTrimmed());
    CHECK(session.undoHistory().size() == 1);
    CHECK(session.undoHistoryAll(true));
    CHECK(markers(doc) == 2);   // as far back as the history still reaches
    return 0;
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QList<QQuickItem*> all_named(QQuickItem* item, const QString& name) {
    QList<QQuickItem*> out;
    if (item->objectName() == name) out.push_back(item);
    for (QQuickItem* c : item->childItems()) out += all_named(c, name);
    return out;
}

void click(QQuickView& view, QQuickItem* item) {
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, p.toPoint());
}

// The rows of the list, in their order on screen, once laid out.
QList<QQuickItem*> rows_on_screen(QQuickItem* root, int count) {
    QList<QQuickItem*> rows;
    (void)QTest::qWaitFor([&]() {
        rows = all_named(root, QStringLiteral("undoHistoryRow"));
        if (rows.size() != count) return false;
        std::sort(rows.begin(), rows.end(),
                  [](QQuickItem* a, QQuickItem* b) { return a->y() < b->y(); });
        return count < 2 || rows[1]->y() > rows[0]->y();
    });
    return rows;
}

int test_window() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(2, api::testing::FakeEngine::engine_commands());
    ModelDocument doc(one_region());
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    model.routeKeysFrom(&view);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));

    std::string why;
    CHECK(session.controller().select_region(10));
    CHECK(session.controller().duplicate(why));
    session.documentUpdated();
    CHECK(add_marker(session, doc, "Verse"));
    CHECK(add_marker(session, doc, "Chorus"));

    QQuickItem* root = view.rootObject();
    QQuickItem* win = named(root, QStringLiteral("undoHistoryWindow"));
    CHECK(win != nullptr && !win->isVisible());

    // Edit > Undo History opens it.
    QQuickItem* button = named(root, QStringLiteral("editMenuButton"));
    CHECK(button != nullptr);
    click(view, button);
    QQuickItem* item = nullptr;
    (void)QTest::qWaitFor([&]() {
        item = named(root, QStringLiteral("editUndoHistory"));
        return item != nullptr && item->parentItem() != nullptr && item->parentItem()->y() > 0;
    });
    CHECK(item != nullptr);
    click(view, item);
    CHECK(session.undoHistoryOpen() && win->isVisible());

    QList<QQuickItem*> rows = rows_on_screen(win, 3);
    CHECK(rows.size() == 3);
    QQuickItem* first = named(rows[0], QStringLiteral("undoHistoryLabel"));
    CHECK(first != nullptr && first->property("text").toString() == QStringLiteral("Duplicate"));

    // A click on the second row undoes it and the third; the third is shown
    // in italics, as redo would bring it back.
    click(view, rows[1]);
    CHECK(doc.history().undo_depth() == 1 && markers(doc) == 0);
    rows = rows_on_screen(win, 3);
    QQuickItem* third = named(rows[2], QStringLiteral("undoHistoryLabel"));
    CHECK(third != nullptr && third->property("font").value<QFont>().italic());
    CHECK(!named(rows[0], QStringLiteral("undoHistoryLabel"))->property("font").value<QFont>().italic());

    click(view, named(win, QStringLiteral("undoHistoryRedoAll")));
    CHECK(doc.history().undo_depth() == 3 && markers(doc) == 2);
    click(view, named(win, QStringLiteral("undoHistoryUndoAll")));
    CHECK(doc.history().undo_depth() == 0 && regions(doc) == 1);

    // A history longer than the list shows (12 rows): the newest step is in
    // view, and a click near the bottom keeps the clicked row in view rather
    // than the list jumping back to step 1.
    for (int i = 0; i < 20; ++i) CHECK(add_marker(session, doc, "M"));
    QQuickItem* list = named(win, QStringLiteral("undoHistoryList"));
    CHECK(list != nullptr);
    const auto in_view = [&](int index) {
        const double top = list->property("contentY").toDouble();
        const double row = 24.0 * index;
        return row >= top - 0.5 && row + 24.0 <= top + list->height() + 0.5;
    };
    CHECK(QTest::qWaitFor([&]() { return in_view(19); }));
    QQuickItem* row17 = nullptr;
    CHECK(QTest::qWaitFor([&]() {
        for (QQuickItem* r : all_named(win, QStringLiteral("undoHistoryRow"))) {
            if (r->property("index").toInt() == 17 && r->isVisible()) row17 = r;
        }
        return row17 != nullptr;
    }));
    click(view, row17);
    CHECK(doc.history().undo_depth() == 17);
    QTest::qWait(50);
    CHECK(in_view(17) && in_view(16));
    // And the other way: the same row, now undone, redone; still in view.
    // The list was rebuilt, so the row is looked up again.
    row17 = nullptr;
    for (QQuickItem* r : all_named(win, QStringLiteral("undoHistoryRow"))) {
        if (r->property("index").toInt() == 17 && r->isVisible()) row17 = r;
    }
    CHECK(row17 != nullptr);
    click(view, row17);
    CHECK(doc.history().undo_depth() == 18);
    QTest::qWait(50);
    CHECK(in_view(17));

    // The close box sends what the menu sent: the window goes.
    click(view, named(win, QStringLiteral("undoHistoryClose")));
    CHECK(!session.undoHistoryOpen() && !win->isVisible());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_rows_and_jumps() || test_trimmed() || test_window()) return 1;
    std::printf("ui undo history: ok\n");
    return 0;
}
