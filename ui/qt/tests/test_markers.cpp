// SPDX-License-Identifier: GPL-3.0-or-later
//
// Markers, as Pro Tools' Memory Locations: marker.add at the insertion
// point, over a selected range, or at the playhead while rolling; the
// names offered; recall moving the selection (never snapped) and the view;
// clearing, one undo step each; the Markers ruler's drawing and hit test.
// Then the real QML: the New Marker dialog from the Edit menu and from the
// keypad Enter key, the Memory Locations window's rows, recall and clear,
// and an Alt-click on the ruler.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
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
#include "scene_item.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "theme.h"
#include "timeline_document.h"
#include "timeline_scene.h"
#include "transport_model.h"

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

const MarkerList& markers(const TimelineDocument& d) { return *d.snapshot().edit->markers; }

const Marker* named_marker(const TimelineDocument& d, const std::string& name) {
    for (const Marker& m : markers(d).markers) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

// --- the controller ---------------------------------------------------------

int test_add_recall_clear() {
    ModelDocument doc(make_empty_session(2, kRate));
    EditController c(doc);
    c.set_viewport(1000, 400);
    std::string why;

    // At the insertion point, offered "Marker 1"; one undo step, "New Marker".
    c.set_insertion(48000);
    CHECK(c.next_marker_name() == "Marker 1");
    auto at = c.marker_place(false, 0);
    CHECK(at.position == 48000 && at.length == 0);
    CHECK(c.add_marker("  Verse \n", at, why));
    CHECK(markers(doc).markers.size() == 1);
    CHECK(markers(doc).markers[0].name == "Verse" && markers(doc).markers[0].length == 0);
    CHECK(doc.undo_label() == "AddMarker");

    // An empty name takes the offered one, the first "Marker N" not taken.
    CHECK(c.add_marker("", c.marker_place(false, 0), why));
    CHECK(named_marker(doc, "Marker 1") != nullptr);
    CHECK(c.next_marker_name() == "Marker 2");

    // A selected range is a selection marker over it.
    c.select_range(1, 96000, 144000);
    at = c.marker_place(false, 0);
    CHECK(at.position == 96000 && at.length == 48000);
    CHECK(c.add_marker("Chorus", at, why));
    const Marker* chorus = named_marker(doc, "Chorus");
    CHECK(chorus != nullptr && chorus->position == 96000 && chorus->length == 48000);

    // Rolling: at the playhead, whatever is selected.
    at = c.marker_place(true, 30000);
    CHECK(at.position == 30000 && at.length == 0);

    // Recall a marker: the insertion point, on the selected track, no range,
    // and not snapped, even in Grid.
    const Id chorus_id = chorus->id;
    const Id verse_id = named_marker(doc, "Verse")->id;
    // The playhead follows: every recall locates there, as Pro Tools does.
    std::vector<Frames> located;
    c.set_locator([&located](Frames f) { located.push_back(f); });
    c.set_mode(EditMode::Grid);
    c.select_track(1);
    CHECK(c.add_marker("Odd", {48001, 0}, why));
    CHECK(c.recall_marker(named_marker(doc, "Odd")->id, why));
    CHECK(c.selection().insertion == 48001 && !c.selection().has_range());
    CHECK(c.selection().track == 1);
    c.set_mode(EditMode::Slip);

    // Recall a selection marker: its range, selected again.
    c.set_insertion(0);
    CHECK(c.recall_marker(chorus_id, why));
    CHECK(c.selection().start == 96000 && c.selection().end == 144000);
    // A selection marker locates to its start; neither is snapped.
    CHECK(located.size() == 2 && located[0] == 48001 && located[1] == 96000);

    // Off screen: the view pages to it, a tenth in from the left.
    CHECK(c.add_marker("Far", {48000 * 600, 0}, why));
    CHECK(c.recall_marker(named_marker(doc, "Far")->id, why));
    const double x = c.view().x_of(48000 * 600);
    CHECK(x > 0 && x < 1000 * 0.2);

    // Clear: one step, undone by Undo; a marker that is gone is refused.
    const std::size_t before = markers(doc).markers.size();
    CHECK(c.remove_marker(verse_id, why));
    CHECK(markers(doc).markers.size() == before - 1);
    CHECK(doc.undo_label() == "RemoveMarker");
    CHECK(!c.remove_marker(verse_id, why) && why.find("no longer") != std::string::npos);
    const std::size_t sent = located.size();
    CHECK(!c.recall_marker(verse_id, why));
    CHECK(located.size() == sent);   // a marker that is gone locates nowhere
    CHECK(c.undo(why));
    CHECK(named_marker(doc, "Verse") != nullptr);
    return 0;
}

// --- the ruler ------------------------------------------------------------------

int test_ruler() {
    ModelDocument doc(make_empty_session(1, kRate));
    EditController c(doc);
    c.set_viewport(1000, 400);
    std::string why;
    // fpp 1200: frame 120000 is x 100, 240000 is x 200.
    CHECK(c.view().fpp == 1200.0);
    CHECK(c.add_marker("Intro", {120000, 0}, why));
    CHECK(c.add_marker("Bridge", {240000, 360000}, why));   // x 200 to 500

    Scene s;
    build_marker_ruler_scene(c.markers(), c.view(), 20, s);
    const SceneRect* r = rect_at(s.rects, 103, 5);   // Intro's flag
    CHECK(r != nullptr && r->argb == theme::kMarker);
    r = rect_at(s.rects, 100, 17);                    // its line, down to the bottom
    CHECK(r != nullptr && r->argb == theme::kMarker);
    r = rect_at(s.rects, 350, 15);                    // Bridge's bar
    CHECK(r != nullptr && r->argb == theme::kMarkerRange);
    r = rect_at(s.rects, 350, 5);                     // above the bar: the ruler
    CHECK(r != nullptr && r->argb == theme::kPanel);
    CHECK(s.labels.size() == 2 && s.labels[0].text == "Intro" && s.labels[1].text == "Bridge");
    // Intro's name stops short of Bridge's flag.
    CHECK(s.labels[0].x + s.labels[0].max_w <= 200);

    // The hit test reads the same geometry.
    const Id intro = markers(doc).markers[0].id, bridge = markers(doc).markers[1].id;
    CHECK(c.marker_at(104) == intro);
    CHECK(c.marker_at(98) == intro);      // within the slop
    CHECK(c.marker_at(350) == bridge);    // on the bar
    CHECK(c.marker_at(150) == kNoId);
    CHECK(c.marker_at(700) == kNoId);
    return 0;
}

// --- the command, through SessionModel ------------------------------------------

int test_command() {
    ModelDocument doc(make_empty_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    QSignalSpy asked(&session, &SessionModel::markerRequested);

    // Stopped, no name: the dialog is asked for, nothing is added yet.
    CommandContext ctx;
    session.controller().set_insertion(48000);
    CHECK(local.dispatch("marker.add", ctx) == DispatchResult::Ok);
    CHECK(asked.size() == 1 && markers(doc).markers.empty());
    CHECK(asked[0][0].toString() == QStringLiteral("Marker 1"));
    CHECK(asked[0][1].toString().startsWith(QStringLiteral("at ")));
    CHECK(asked[0][2].toBool() == false);

    // The dialog's answer: the name as the argument.
    ctx.argument = "Verse";
    CHECK(local.dispatch("marker.add", ctx) == DispatchResult::Ok);
    CHECK(named_marker(doc, "Verse") != nullptr && named_marker(doc, "Verse")->position == 48000);
    CHECK(session.undoText() == QStringLiteral("Undo New Marker"));
    QVariantList rows = session.markers();
    CHECK(rows.size() == 1 && rows[0].toMap()[QStringLiteral("name")] == QStringLiteral("Verse"));

    // Rolling: no dialog, named at once, at the playhead.
    TransportSnapshot t;
    t.state = TransportMode::Rolling;
    t.playhead_frames = 240000;
    session.onTransport(t, true);
    ctx.argument.clear();
    CHECK(local.dispatch("marker.add", ctx) == DispatchResult::Ok);
    CHECK(asked.size() == 1);
    const Marker* m = named_marker(doc, "Marker 1");
    CHECK(m != nullptr && m->position == 240000);

    // The window's rows: recall and clear by row.
    t.state = TransportMode::Stopped;
    session.onTransport(t, true);
    CHECK(session.recallMarker(1));
    CHECK(session.controller().selection().insertion == 240000);
    CHECK(session.removeMarker(0));
    CHECK(markers(doc).markers.size() == 1);
    CHECK(session.undoText() == QStringLiteral("Undo Clear Marker"));
    CHECK(!session.removeMarker(5));

    local.dispatch("window.memory_locations", CommandContext{});
    CHECK(session.memoryLocationsOpen());

    // The length column follows the main counter, not always min:sec.
    std::string range_why;
    session.controller().select_range(1, 96000, 144000);
    CHECK(session.controller().add_marker("Range", session.controller().marker_place(false, 0), range_why));
    auto length_of = [&]() {
        for (const QVariant& r : session.markers()) {
            if (r.toMap()[QStringLiteral("name")] == QStringLiteral("Range"))
                return r.toMap()[QStringLiteral("length")].toString();
        }
        return QString();
    };
    CHECK(session.mainCounterFormat() == QStringLiteral("Bars|Beats"));
    CHECK(length_of() == QStringLiteral("0|2|000"));
    session.cycleMainCounter();
    CHECK(length_of() == QStringLiteral("0:01.000"));
    session.cycleMainCounter();
    CHECK(length_of() == QStringLiteral("00:00:01:00"));
    session.cycleMainCounter();
    CHECK(length_of() == QStringLiteral("48000"));
    return 0;
}

// --- the real QML -------------------------------------------------------------

void key(QQuickView& view, int k, Qt::KeyboardModifiers m) {
    QKeyEvent press(QEvent::KeyPress, k, m, 0, 0, 0);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m, 0, 0, 0);
    QCoreApplication::sendEvent(&view, &release);
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

QQuickItem* menu_item(QQuickItem* root, const QString& name) {
    QQuickItem* item = nullptr;
    (void)QTest::qWaitFor([&]() {
        item = named(root, name);
        return item != nullptr && item->parentItem() != nullptr && item->parentItem()->y() > 0;
    });
    return item;
}

void click_at(QQuickView& view, QQuickItem* item, QPointF at, Qt::KeyboardModifiers m) {
    QTest::mouseClick(&view, Qt::LeftButton, m, item->mapToScene(at).toPoint());
}

void click(QQuickView& view, QQuickItem* item) {
    click_at(view, item, QPointF(item->width() / 2, item->height() / 2), Qt::NoModifier);
}

int test_window() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(2, api::testing::FakeEngine::engine_commands());
    ModelDocument doc(make_empty_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    link_marker_locate(model, session);   // as main.cpp wires them
    const auto last_locate = [&engine]() -> std::string {
        for (auto it = engine.sent.rbegin(); it != engine.sent.rend(); ++it) {
            if (it->command == "transport.locate") return it->argument;
        }
        return "none";
    };

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
    QQuickItem* root = view.rootObject();

    // Edit > New Marker… opens the dialog; OK adds the typed name.
    session.controller().set_insertion(96000);
    click(view, named(root, QStringLiteral("editMenuButton")));
    QQuickItem* item = menu_item(root, QStringLiteral("editNewMarker"));
    CHECK(item != nullptr);
    click(view, item);
    QQuickItem* panel = named(root, QStringLiteral("markerPanel"));
    CHECK(panel != nullptr && panel->isVisible());
    QQuickItem* input = named(root, QStringLiteral("markerInput"));
    CHECK(input != nullptr && input->property("text").toString() == QStringLiteral("Marker 1"));
    input->setProperty("text", QStringLiteral("Verse"));
    click(view, named(root, QStringLiteral("markerOk")));
    CHECK(!panel->isVisible());
    CHECK(named_marker(doc, "Verse") != nullptr && named_marker(doc, "Verse")->position == 96000);

    // Pro Tools' key: Enter on the numeric keypad opens it too; Return in the
    // field takes the offered name.
    session.controller().set_insertion(480000);
    key(view, Qt::Key_Enter, Qt::KeypadModifier);
    CHECK(panel->isVisible());
    CHECK(QTest::qWaitFor([&]() { return input->hasActiveFocus(); }));
    key(view, Qt::Key_Return, Qt::NoModifier);
    CHECK(!panel->isVisible());
    CHECK(named_marker(doc, "Marker 1") != nullptr);

    // The Markers ruler draws them; Ctrl+5 on the keypad opens the window.
    QQuickItem* ruler = named(root, QStringLiteral("markerRuler"));
    CHECK(ruler != nullptr);
    auto* marker_ruler = qobject_cast<MarkerRulerItem*>(ruler);
    CHECK(marker_ruler != nullptr);
    CHECK(QTest::qWaitFor([&]() { return marker_ruler->content().labels.size() == 2; }));

    key(view, Qt::Key_5, Qt::ControlModifier | Qt::KeypadModifier);
    QQuickItem* win = named(root, QStringLiteral("memoryLocationsWindow"));
    CHECK(win != nullptr && session.memoryLocationsOpen() && win->isVisible());
    QList<QQuickItem*> rows;
    CHECK(QTest::qWaitFor([&]() {
        rows = all_named(win, QStringLiteral("memoryLocationRow"));
        return rows.size() == 2;
    }));
    std::sort(rows.begin(), rows.end(), [](QQuickItem* a, QQuickItem* b) { return a->y() < b->y(); });
    CHECK(named(rows[0], QStringLiteral("memoryLocationName"))->property("text").toString() ==
          QStringLiteral("Verse"));

    // A click on the second row recalls it: the insertion point moves there.
    session.controller().set_insertion(0);
    click_at(view, rows[1], QPointF(60, 12), Qt::NoModifier);
    CHECK(session.controller().selection().insertion == 480000);
    // And the engine's playhead is sent there.
    CHECK(last_locate() == "480000");

    // Its × clears it, one undo step.
    click(view, named(rows[1], QStringLiteral("memoryLocationRemove")));
    CHECK(markers(doc).markers.size() == 1 && named_marker(doc, "Verse") != nullptr);
    CHECK(session.undoText() == QStringLiteral("Undo Clear Marker"));

    // On the ruler: a click on Verse's flag recalls it, Alt-click clears it.
    session.controller().set_insertion(0);
    const double vx = session.controller().view().x_of(96000);
    click_at(view, ruler, QPointF(vx + 3, 6), Qt::NoModifier);
    CHECK(session.controller().selection().insertion == 96000);
    CHECK(last_locate() == "96000");
    const std::size_t sent = engine.sent.size();
    click_at(view, ruler, QPointF(vx + 3, 6), Qt::AltModifier);
    CHECK(markers(doc).markers.empty());
    // Clearing a marker is not recalling it: nothing is located.
    CHECK(engine.sent.size() == sent);

    click(view, named(win, QStringLiteral("memoryLocationsClose")));
    CHECK(!session.memoryLocationsOpen() && !win->isVisible());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_add_recall_clear() || test_ruler() || test_command() || test_window()) return 1;
    std::printf("ui markers: ok\n");
    return 0;
}
