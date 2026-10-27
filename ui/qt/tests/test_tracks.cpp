// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pro Tools' Track menu (ADR-0022): new tracks below the selection on free
// inputs, duplicate with the playlist and plug-ins, delete, move up and
// down, each one undo step with the selection following; the commands, the
// count asked for, the engine's track count and a take's armed track
// refused before anything is sent, the latter held. Then the real QML: the
// Track menu from its button and from a right-click on a track header, the
// New Tracks dialog, and Pro Tools' Duplicate Tracks key.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <cstdio>
#include <string>
#include <vector>

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

std::vector<std::string> names(const TimelineDocument& d) {
    std::vector<std::string> out;
    for (const auto& t : d.snapshot().edit->tracks) out.push_back(t->name);
    return out;
}

std::uint16_t input_of(const TimelineDocument& d, std::size_t row) {
    const Snapshot& s = d.snapshot();
    return s.mix.find(s.edit->tracks[row]->id)->input.front();
}

// --- the controller ---------------------------------------------------------

int test_controller() {
    ModelDocument doc(make_demo_session(3, kRate));
    EditController c(doc);
    std::string why;
    const std::vector<std::string> start = names(doc);

    // Two below the first track, on the inputs nobody uses, one step.
    c.select_track(0);
    CHECK(c.new_tracks(2, why));
    CHECK((names(doc) == std::vector<std::string>{start[0], "Audio 1", "Audio 2", start[1], start[2]}));
    CHECK(input_of(doc, 1) == 3 && input_of(doc, 2) == 4);
    CHECK(c.selection().track == 1);
    CHECK(doc.undo_label() == "New Tracks");
    CHECK(c.undo(why));
    CHECK(names(doc) == start);
    CHECK(!c.new_tracks(0, why) && why.find("1 to") != std::string::npos);

    // Below the last track: at the end, with no reorder.
    c.select_track(2);
    CHECK(c.new_tracks(1, why));
    CHECK(names(doc).back() == "Audio 1" && c.selection().track == 3);
    CHECK(c.undo(why));

    // Duplicate: below, with its regions, the copy selected.
    const std::size_t regions = doc.snapshot().edit->tracks[1]->playlists[0]->regions.size();
    CHECK(regions > 0);
    CHECK(c.duplicate_track(1, why));
    CHECK(names(doc)[2] == start[1] + ".dup1");
    CHECK(doc.snapshot().edit->tracks[2]->playlists[0]->regions.size() == regions);
    CHECK(c.selection().track == 2);
    CHECK(doc.undo_label() == "DuplicateTrack");

    // Move: the selection goes with it; past the top is refused by name.
    CHECK(c.move_track(2, -1, why));
    CHECK(names(doc)[1] == start[1] + ".dup1" && c.selection().track == 1);
    CHECK(c.move_track(1, -1, why));
    CHECK(!c.move_track(0, -1, why));
    CHECK(why == "\"" + start[1] + ".dup1\" is already the top track");
    CHECK(!c.move_track(3, 1, why) && why.find("bottom") != std::string::npos);

    // Delete: the next track takes the selection.
    CHECK(c.delete_track(0, why));
    CHECK(names(doc) == start && c.selection().track == 0);
    CHECK(doc.undo_label() == "RemoveTrack");
    CHECK(c.undo(why));
    CHECK(names(doc)[0] == start[1] + ".dup1");
    CHECK(!c.delete_track(9, why) && why == "there is no track 10");
    return 0;
}

// --- the commands -------------------------------------------------------------

int test_commands() {
    ModelDocument doc(make_empty_session(3, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    std::vector<bool> armed{false, true, false};
    int engine_tracks = 6;
    session.setEngineTracks([&armed]() { return armed; }, [&engine_tracks]() { return engine_tracks; });
    QSignalSpy asked(&session, &SessionModel::newTracksRequested);
    std::string why;

    // No count: the dialog is asked for, nothing made yet.
    CommandContext ctx;
    CHECK(local.dispatch("track.new", ctx) == DispatchResult::Ok);
    CHECK(asked.size() == 1 && doc.snapshot().edit->tracks.size() == 3);
    ctx.argument = "two";
    CHECK(local.dispatch("track.new", ctx, why) == DispatchResult::Failed);
    CHECK(why.find("not a number of tracks") != std::string::npos);
    ctx.argument = "2";
    CHECK(local.dispatch("track.new", ctx) == DispatchResult::Ok);
    CHECK(doc.snapshot().edit->tracks.size() == 5);
    CHECK(session.undoText() == QStringLiteral("Undo New Tracks"));
    // Past the engine's six tracks.
    CHECK(local.dispatch("track.new", ctx, why) == DispatchResult::Failed);
    CHECK(why.find("delete a track before adding another") != std::string::npos);
    CHECK(doc.snapshot().edit->tracks.size() == 5);

    CommandContext on;
    on.target = 4;
    CHECK(local.dispatch("track.delete", on) == DispatchResult::Ok);
    CHECK(session.undoText() == QStringLiteral("Undo Delete Track"));
    on.target = 0;
    CHECK(local.dispatch("track.duplicate", on) == DispatchResult::Ok);
    CHECK(session.undoText() == QStringLiteral("Undo Duplicate Track"));
    CHECK(names(doc)[1] == "Audio 1.dup1");
    CHECK(local.dispatch("track.move_down", on) == DispatchResult::Ok);
    CHECK(names(doc)[1] == "Audio 1");

    // A take records row 2: what would delete or move it is refused here,
    // and the reason held; what leaves it alone goes through.
    armed = {false, true, false, false, false};
    TransportSnapshot t;
    t.state = TransportMode::Recording;
    session.onTransport(t, true);
    const std::vector<std::string> before = names(doc);
    on.target = 1;
    CHECK(local.dispatch("track.delete", on, why) == DispatchResult::Failed);
    CHECK(why == "\"Audio 1\" is recording: stop recording before deleting it");
    CHECK(session.trackRefusal() == QString::fromStdString(why));
    on.target = 0;
    CHECK(local.dispatch("track.move_down", on, why) == DispatchResult::Failed);
    CHECK(why.find("before moving it") != std::string::npos);
    CHECK(names(doc) == before);
    session.dismissTrackRefusal();
    CHECK(session.trackRefusal().isEmpty());
    CHECK(local.dispatch("track.delete", on, why) == DispatchResult::Failed);
    on.target = 3;
    CHECK(local.dispatch("track.move_down", on) == DispatchResult::Ok);
    // A command that went through clears it.
    CHECK(session.trackRefusal().isEmpty());
    return 0;
}

// --- the real QML -------------------------------------------------------------

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QQuickItem* menu_item(QQuickItem* root, const QString& name) {
    QQuickItem* item = nullptr;
    (void)QTest::qWaitFor([&]() {
        item = named(root, name);
        return item != nullptr && item->isVisible() && item->parentItem() != nullptr &&
               item->parentItem()->y() > 0;
    });
    return item;
}

void click(QQuickView& view, QQuickItem* item, Qt::MouseButton b = Qt::LeftButton) {
    QTest::mouseClick(&view, b, Qt::NoModifier,
                      item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

void key(QQuickView& view, int k, Qt::KeyboardModifiers m) {
    QKeyEvent press(QEvent::KeyPress, k, m, 0, 0, 0);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m, 0, 0, 0);
    QCoreApplication::sendEvent(&view, &release);
}

int test_window() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(8, api::testing::FakeEngine::engine_commands());
    ModelDocument doc(make_empty_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    std::vector<bool> armed(8, false);
    session.setEngineTracks([&armed]() { return armed; }, []() { return 8; });

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

    // Track > New… asks how many; Create makes them below the selection.
    session.selectTrack(0);
    click(view, named(root, QStringLiteral("trackMenuButton")));
    QQuickItem* item = menu_item(root, QStringLiteral("trackNew"));
    CHECK(item != nullptr);
    click(view, item);
    QQuickItem* panel = named(root, QStringLiteral("newTracksPanel"));
    CHECK(panel != nullptr && panel->isVisible());
    QQuickItem* input = named(root, QStringLiteral("newTracksInput"));
    CHECK(input != nullptr && input->property("text").toString() == QStringLiteral("1"));
    input->setProperty("text", QStringLiteral("2"));
    click(view, named(root, QStringLiteral("newTracksCreate")));
    CHECK(!panel->isVisible());
    CHECK((names(doc) == std::vector<std::string>{"Audio 1", "Audio 3", "Audio 4", "Audio 2"}));

    // A right-click on a header selects that track and opens the menu on it.
    QQuickItem* edit = named(root, QStringLiteral("editWindow"));
    CHECK(edit != nullptr);
    QQuickItem* menu = named(root, QStringLiteral("trackMenu"));
    CHECK(menu != nullptr && !menu->isVisible());
    QQuickItem* lane = nullptr;
    (void)QTest::qWaitFor([&]() {
        lane = named(root, QStringLiteral("trackHeader3"));
        return lane != nullptr;
    });
    CHECK(lane != nullptr);
    // Its top-left corner, clear of the header's buttons.
    QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                      lane->mapToScene(QPointF(8, 8)).toPoint());
    CHECK(session.selectedTrack() == 3);
    CHECK(QTest::qWaitFor([&]() { return menu->isVisible(); }));
    click(view, menu_item(root, QStringLiteral("trackMoveUp")));
    CHECK(names(doc)[2] == "Audio 2" && session.selectedTrack() == 2);

    // Pro Tools' Duplicate Tracks, Option+Shift+D, on the selected track.
    key(view, Qt::Key_D, Qt::AltModifier | Qt::ShiftModifier);
    CHECK(QTest::qWaitFor([&]() { return names(doc).size() == 5; }));
    CHECK(names(doc)[3] == "Audio 2.dup1");

    // Recording on the copy's row: Delete is refused and the reason held in
    // the footer until dismissed.
    armed[3] = true;
    TransportSnapshot t;
    t.state = TransportMode::Recording;
    session.onTransport(t, true);
    click(view, named(root, QStringLiteral("trackMenuButton")));
    click(view, menu_item(root, QStringLiteral("trackDelete")));
    CHECK(names(doc).size() == 5);
    QQuickItem* held = named(root, QStringLiteral("trackRefusal"));
    CHECK(held != nullptr && QTest::qWaitFor([&]() { return held->isVisible(); }));
    click(view, named(root, QStringLiteral("dismissTrackRefusal")));
    CHECK(QTest::qWaitFor([&]() { return !held->isVisible(); }));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_controller() || test_commands() || test_window()) return 1;
    std::printf("ui tracks: ok\n");
    return 0;
}
