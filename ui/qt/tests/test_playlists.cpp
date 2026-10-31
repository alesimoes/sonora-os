// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playlist selector (ADR-0026): New, Duplicate, choose, Rename and
// Delete Unused on a track, each one undo step named for the command; the
// names offered; a take's armed track refused a switch before anything is
// sent, the reason held. Then the real QML: the selector on a header, its
// menu, the Rename dialog.
#include <QGuiApplication>
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

const TrackEdit& track(const TimelineDocument& d, std::size_t row) {
    return *d.snapshot().edit->tracks[row];
}

std::size_t active_regions(const TimelineDocument& d, std::size_t row) {
    for (const auto& p : track(d, row).playlists)
        if (p->id == track(d, row).active_playlist) return p->regions.size();
    return 0;
}

// --- the controller ---------------------------------------------------------

int test_controller() {
    ModelDocument doc(make_demo_session(3, kRate));
    EditController c(doc);
    std::string why;
    const std::string first = c.active_playlist_name(1);
    const std::string base = track(doc, 1).name;
    const std::size_t regions = active_regions(doc, 1);
    CHECK(regions > 0);

    // New: empty, active, "<track>.02", one step.
    CHECK(c.new_playlist(1, why));
    CHECK(c.active_playlist_name(1) == base + ".02");
    CHECK(active_regions(doc, 1) == 0);
    CHECK((c.playlist_names(1) == std::vector<std::string>{first, base + ".02"}));
    CHECK(doc.undo_label() == "New Playlist");
    CHECK(c.undo(why));
    CHECK(c.active_playlist_name(1) == first && c.playlist_names(1).size() == 1);

    // Duplicate: the regions copied, the copy active.
    CHECK(c.duplicate_playlist(1, why));
    CHECK(c.active_playlist_name(1) == base + ".02");
    CHECK(active_regions(doc, 1) == regions);
    CHECK(doc.undo_label() == "Duplicate Playlist");

    // Choose by name; the one already active changes nothing.
    CHECK(c.choose_playlist(1, first, why));
    CHECK(c.active_playlist_name(1) == first);
    CHECK(doc.undo_label() == "SetActivePlaylist");
    CHECK(c.choose_playlist(1, first, why));
    CHECK(doc.undo_label() == "SetActivePlaylist");
    CHECK(!c.choose_playlist(1, "Nope", why));
    CHECK(why == "\"" + base + "\" has no playlist named \"Nope\"");

    // Rename the active one; a name another playlist has is refused.
    CHECK(!c.rename_playlist(1, base + ".02", why));
    CHECK(why.find("already named") != std::string::npos);
    CHECK(!c.rename_playlist(1, "", why));
    CHECK(c.rename_playlist(1, "Lead take", why));
    CHECK(c.active_playlist_name(1) == "Lead take");

    // Delete Unused: only the active one stays; undo brings the rest back.
    CHECK(c.delete_unused_playlists(1, why));
    CHECK((c.playlist_names(1) == std::vector<std::string>{"Lead take"}));
    CHECK(doc.undo_label() == "Delete Unused Playlists");
    CHECK(!c.delete_unused_playlists(1, why));
    CHECK(why == "\"" + base + "\" has no unused playlists");
    CHECK(c.undo(why));
    CHECK(c.playlist_names(1).size() == 2);
    CHECK(!c.new_playlist(9, why) && why == "there is no track 10");
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
    session.setEngineTracks([&armed]() { return armed; }, []() { return 3; });
    QSignalSpy rename(&session, &SessionModel::playlistRenameRequested);
    std::string why;

    CommandContext on;
    on.target = 1;
    CHECK(local.dispatch("track.playlist_new", on) == DispatchResult::Ok);
    CHECK(session.playlistNames().at(1) == QStringLiteral("Audio 2.02"));
    CHECK(session.undoText() == QStringLiteral("Undo New Playlist"));
    CHECK((session.trackPlaylists(1) == QStringList{"Audio 2.01", "Audio 2.02"}));
    on.argument = "Audio 2.01";
    CHECK(local.dispatch("track.playlist_choose", on) == DispatchResult::Ok);
    CHECK(session.playlistNames().at(1) == QStringLiteral("Audio 2.01"));
    CHECK(session.undoText() == QStringLiteral("Undo Choose Playlist"));

    // Rename with no name asks the window, offering the active one's.
    on.argument.clear();
    CHECK(local.dispatch("track.playlist_rename", on) == DispatchResult::Ok);
    CHECK(rename.size() == 1 && rename.at(0).at(0).toInt() == 1 &&
          rename.at(0).at(1).toString() == QStringLiteral("Audio 2.01"));
    on.argument = "Verse";
    CHECK(local.dispatch("track.playlist_rename", on) == DispatchResult::Ok);
    CHECK(session.undoText() == QStringLiteral("Undo Rename Playlist"));
    on.argument.clear();
    CHECK(local.dispatch("track.playlist_duplicate", on) == DispatchResult::Ok);
    CHECK(session.undoText() == QStringLiteral("Undo Duplicate Playlist"));

    // A take records row 2: no switch there, held; another row may switch,
    // and renaming or deleting what is not active goes through.
    TransportSnapshot t;
    t.state = TransportMode::Recording;
    session.onTransport(t, true);
    const QString before = session.playlistNames().at(1);
    on.argument = "Verse";
    CHECK(local.dispatch("track.playlist_choose", on, why) == DispatchResult::Failed);
    CHECK(why == "\"Audio 2\" is recording: stop recording before choosing another playlist");
    CHECK(session.trackRefusal() == QString::fromStdString(why));
    on.argument.clear();
    CHECK(local.dispatch("track.playlist_new", on, why) == DispatchResult::Failed);
    CHECK(local.dispatch("track.playlist_duplicate", on, why) == DispatchResult::Failed);
    CHECK(session.playlistNames().at(1) == before);
    on.argument = before.toStdString();
    CHECK(local.dispatch("track.playlist_choose", on) == DispatchResult::Ok);  // already active
    on.argument = "Chorus";
    CHECK(local.dispatch("track.playlist_rename", on) == DispatchResult::Ok);
    on.argument.clear();
    CHECK(local.dispatch("track.playlist_delete_unused", on) == DispatchResult::Ok);
    CHECK(session.trackPlaylists(1).size() == 1);
    on.target = 0;
    CHECK(local.dispatch("track.playlist_new", on) == DispatchResult::Ok);
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

void click(QQuickView& view, QQuickItem* item) {
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
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

    // The selector shows the active playlist; New from its menu makes one.
    QQuickItem* selector = nullptr;
    (void)QTest::qWaitFor([&]() {
        selector = named(root, QStringLiteral("playlistSelector1"));
        return selector != nullptr;
    });
    CHECK(selector != nullptr);
    CHECK(selector->property("text").toString().startsWith(QStringLiteral("Audio 2.01")));
    QQuickItem* menu = named(root, QStringLiteral("playlistMenu"));
    CHECK(menu != nullptr && !menu->isVisible());
    click(view, selector);
    CHECK(QTest::qWaitFor([&]() { return menu->isVisible(); }));
    CHECK(session.selectedTrack() == 1);
    click(view, menu_item(root, QStringLiteral("playlistNew")));
    CHECK(!menu->isVisible());
    CHECK(session.playlistNames().at(1) == QStringLiteral("Audio 2.02"));
    CHECK(QTest::qWaitFor([&]() {
        return selector->property("text").toString().startsWith(QStringLiteral("Audio 2.02"));
    }));

    // The menu lists both; choosing the first makes it active again.
    click(view, selector);
    CHECK(QTest::qWaitFor([&]() { return menu->isVisible(); }));
    QQuickItem* choose = menu_item(root, QStringLiteral("playlistChoose0"));
    CHECK(choose != nullptr && choose->property("text").toString().endsWith(QStringLiteral("Audio 2.01")));
    CHECK(menu_item(root, QStringLiteral("playlistChoose1"))->property("text").toString().startsWith(
        QStringLiteral("✓")));
    click(view, choose);
    CHECK(session.playlistNames().at(1) == QStringLiteral("Audio 2.01"));

    // Rename… asks, offering the name; Rename sends the new one.
    click(view, selector);
    CHECK(QTest::qWaitFor([&]() { return menu->isVisible(); }));
    click(view, menu_item(root, QStringLiteral("playlistRename")));
    QQuickItem* panel = named(root, QStringLiteral("playlistRenamePanel"));
    CHECK(panel != nullptr && QTest::qWaitFor([&]() { return panel->isVisible(); }));
    QQuickItem* input = named(root, QStringLiteral("playlistRenameInput"));
    CHECK(input != nullptr && input->property("text").toString() == QStringLiteral("Audio 2.01"));
    // Another playlist's name: refused, the dialog stays open.
    input->setProperty("text", QStringLiteral("Audio 2.02"));
    click(view, named(root, QStringLiteral("playlistRenameOk")));
    CHECK(panel->isVisible());
    input->setProperty("text", QStringLiteral("Verse"));
    click(view, named(root, QStringLiteral("playlistRenameOk")));
    CHECK(!panel->isVisible());
    CHECK(session.playlistNames().at(1) == QStringLiteral("Verse"));

    // Delete Unused leaves the active one only.
    click(view, selector);
    CHECK(QTest::qWaitFor([&]() { return menu->isVisible(); }));
    click(view, menu_item(root, QStringLiteral("playlistDeleteUnused")));
    CHECK(session.trackPlaylists(1) == QStringList{QStringLiteral("Verse")});
    CHECK(session.undoText() == QStringLiteral("Undo Delete Unused Playlists"));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_controller() || test_commands() || test_window()) return 1;
    std::printf("ui playlists: ok\n");
    return 0;
}
