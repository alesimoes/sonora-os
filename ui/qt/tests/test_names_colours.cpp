// SPDX-License-Identifier: GPL-3.0-or-later
//
// Names and colours (ADR-0023, update of 2026-11-02): a track or region
// renamed, a palette colour on a track and on a region, each one undo step
// named for the command; a track's colour stored on it, so a move keeps it
// (design review F21) and the Edit and Mix windows draw it from there; the
// refusals said by name. Then the real QML: the double-click on a track
// name and on a region, the Name dialog with its held refusal, the palette.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <cstdio>
#include <cstdlib>
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
#include "theme.h"
#include "timeline_document.h"
#include "timeline_scene.h"

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

const Region& first_region(const TimelineDocument& d, std::size_t row) {
    const TrackEdit& t = track(d, row);
    for (const auto& p : t.playlists)
        if (p->id == t.active_playlist) return p->regions.front();
    return t.playlists.front()->regions.front();
}

// --- the controller ---------------------------------------------------------

int test_controller() {
    ModelDocument doc(make_demo_session(4, kRate));
    EditController c(doc);
    std::string why;

    // Each track has its own stored colour; moving one keeps every colour
    // with its track instead of recolouring the rows below it (F21).
    const Id second = track(doc, 1).id;
    const int colour = track(doc, 1).colour;
    CHECK(colour == 1 && track(doc, 2).colour == 2);
    CHECK(c.move_track(1, 1, why));
    CHECK(track(doc, 2).id == second && track(doc, 2).colour == colour);
    CHECK(track(doc, 1).colour == 2);
    CHECK(c.undo(why));

    // Rename: one step, trimmed; refused empty or as another track is named.
    CHECK(c.rename_track(1, "  Lead Vox ", why));
    CHECK(track(doc, 1).name == "Lead Vox");
    CHECK(doc.undo_label() == "SetTrackName");
    CHECK(!c.rename_track(1, "   ", why) && why == "a track needs a name; \"Lead Vox\" keeps its own");
    CHECK(!c.rename_track(2, "Lead Vox", why));
    CHECK(why == "another track is already named \"Lead Vox\"");
    CHECK(!c.rename_track(9, "x", why) && why == "there is no track 10");

    // Colour: a palette index, out of range refused; several tracks at
    // once are one step.
    CHECK(c.set_track_colour(0, 1, 12, why));
    CHECK(track(doc, 0).colour == 12);
    CHECK(!c.set_track_colour(0, 1, kColours, why) && why == "the palette has colours 1 to 16");
    CHECK(c.set_track_colour(1, 3, 5, why));
    CHECK(track(doc, 1).colour == 5 && track(doc, 2).colour == 5 && track(doc, 3).colour == 5);
    CHECK(doc.undo_label() == "Colour Tracks");
    CHECK(c.undo(why));
    CHECK(track(doc, 1).colour == 1 && track(doc, 3).colour == 3);

    // A region: nothing to act on until one is selected.
    CHECK(!c.rename_region("Verse", why));
    CHECK(why == "select a region first: click it with the Grabber or the Smart Tool");
    CHECK(!c.set_region_colour(3, why));
    const Id r = first_region(doc, 1).id;
    CHECK(c.select_region(r));
    CHECK(c.rename_region("Verse", why));
    CHECK(first_region(doc, 1).name == "Verse");
    CHECK(!c.rename_region(" ", why) && why == "a region needs a name; \"Verse\" keeps its own");
    CHECK(first_region(doc, 1).colour == kTrackColour);
    CHECK(EditController::region_colour(track(doc, 1), first_region(doc, 1)) == 1);
    CHECK(c.set_region_colour(9, why));
    CHECK(EditController::region_colour(track(doc, 1), first_region(doc, 1)) == 9);
    CHECK(c.set_region_colour(kTrackColour, why));
    CHECK(first_region(doc, 1).colour == kTrackColour);
    CHECK(!c.set_region_colour(16, why));
    return 0;
}

// --- the commands -------------------------------------------------------------

int test_commands() {
    ModelDocument doc(make_demo_session(3, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    QSignalSpy names(&session, &SessionModel::nameRequested);
    QSignalSpy colours(&session, &SessionModel::colourRequested);
    std::string why;

    CommandContext on;
    on.target = 1;
    // No argument: the window asks, offering what is there now.
    CHECK(local.dispatch("track.rename", on) == DispatchResult::Ok);
    CHECK(names.size() == 1 && names.at(0).at(0).toInt() == 1 &&
          names.at(0).at(1).toString() == session.trackNames().at(1));
    CHECK(local.dispatch("track.colour", on) == DispatchResult::Ok);
    CHECK(colours.size() == 1 && colours.at(0).at(0).toInt() == 1 && colours.at(0).at(1).toInt() == 1);
    on.argument = "Kick In";
    CHECK(local.dispatch("track.rename", on) == DispatchResult::Ok);
    CHECK(session.trackNames().at(1) == QStringLiteral("Kick In"));
    CHECK(session.undoText() == QStringLiteral("Undo Rename Track"));
    // The palette is numbered from 1, as the operator sees it.
    on.argument = "16";
    CHECK(local.dispatch("track.colour", on) == DispatchResult::Ok);
    CHECK(session.trackColours().at(1) == 15);
    CHECK(session.undoText() == QStringLiteral("Undo Colour Track"));
    for (const char* bad : {"0", "17", "red", "-1", "123"}) {
        on.argument = bad;
        CHECK(local.dispatch("track.colour", on, why) == DispatchResult::Failed);
        CHECK(why == "\"" + std::string(bad) +
                         "\" is not a palette colour; give 1 to 16. Nothing was coloured.");
    }
    CHECK(session.trackColours().at(1) == 15);

    // A region command acts on the selected region, never on the one the
    // insertion point happens to be in.
    CommandContext none;
    CHECK(local.dispatch("region.rename", none, why) == DispatchResult::Failed);
    CHECK(why == "select a region first: click it with the Grabber or the Smart Tool");
    CHECK(names.size() == 1);
    session.controller().select_region(first_region(doc, 2).id);
    CHECK(local.dispatch("region.rename", none) == DispatchResult::Ok);
    CHECK(names.size() == 2 && names.at(1).at(0).toInt() == -1 &&
          names.at(1).at(1).toString() == QString::fromStdString(first_region(doc, 2).name));
    CHECK(local.dispatch("region.colour", none) == DispatchResult::Ok);
    CHECK(colours.size() == 2 && colours.at(1).at(0).toInt() == -1 &&
          colours.at(1).at(1).toInt() == -1);
    none.argument = "Chorus";
    CHECK(local.dispatch("region.rename", none) == DispatchResult::Ok);
    CHECK(session.undoText() == QStringLiteral("Undo Rename Region"));
    none.argument = "4";
    CHECK(local.dispatch("region.colour", none) == DispatchResult::Ok);
    CHECK(first_region(doc, 2).colour == 3);
    CHECK(session.undoText() == QStringLiteral("Undo Colour Region"));
    none.argument.clear();
    CHECK(local.dispatch("region.colour", none) == DispatchResult::Ok);
    CHECK(colours.at(2).at(1).toInt() == 3);
    none.argument = "track";
    CHECK(local.dispatch("region.colour", none) == DispatchResult::Ok);
    CHECK(first_region(doc, 2).colour == kTrackColour);
    none.argument = "0";
    CHECK(local.dispatch("region.colour", none, why) == DispatchResult::Failed);
    CHECK(why == "\"0\" is not a palette colour; give 1 to 16, or \"track\". Nothing was coloured.");

    // The timeline draws a region in its own colour, else its track's.
    none.argument = "11";
    CHECK(local.dispatch("region.colour", none) == DispatchResult::Ok);
    const Region& coloured = first_region(doc, 2);
    CHECK(EditController::region_colour(track(doc, 2), coloured) == 10);
    CHECK(theme::region_colour(10) == theme::kRegionPalette[10]);
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

// named(), failing the test by name when the item is not there.
QQuickItem* must(QQuickItem* root, const QString& name) {
    QQuickItem* item = named(root, name);
    if (item == nullptr) {
        std::fprintf(stderr, "FAIL: no item named %s\n", qPrintable(name));
        std::exit(1);
    }
    return item;
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

QPoint centre(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

void click(QQuickView& view, QQuickItem* item) {
    if (item == nullptr) {
        std::fprintf(stderr, "FAIL: clicked an item that is not there\n");
        std::exit(1);
    }
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, centre(item));
}

int test_window() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(8, api::testing::FakeEngine::engine_commands());
    ModelDocument doc(make_demo_session(3, kRate));
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

    // The header's colour bar is the track's stored colour.
    // The headers are a Repeater over the track names, made again whenever
    // the session's content changes, so an item is looked up each time.
    const auto bar = [&]() { return named(root, QStringLiteral("trackColour1")); };
    const auto bar_is = [&](int colour) {
        QQuickItem* b = bar();
        return b != nullptr &&
               b->property("color").value<QColor>() == QColor::fromRgba(theme::region_colour(colour));
    };
    CHECK(QTest::qWaitFor([&]() { return bar_is(1); }));

    // A double-click on the name opens Rename Track with the name selected.
    QQuickItem* header = named(root, QStringLiteral("trackHeader1"));
    CHECK(header != nullptr);
    const QPoint on_name = header->mapToScene(QPointF(theme::kSpace3 + 4, 12)).toPoint();
    QTest::mouseDClick(&view, Qt::LeftButton, Qt::NoModifier, on_name);
    QQuickItem* panel = named(root, QStringLiteral("namePanel"));
    CHECK(panel != nullptr && QTest::qWaitFor([&]() { return panel->isVisible(); }));
    CHECK(must(root, QStringLiteral("nameTitle"))->property("text").toString() ==
          QStringLiteral("Rename Track"));
    QQuickItem* input = named(root, QStringLiteral("nameInput"));
    CHECK(input->property("text").toString() == session.trackNames().at(1));
    // Another track's name: refused, the dialog stays open and says why.
    input->setProperty("text", session.trackNames().at(0));
    click(view, must(root, QStringLiteral("nameOk")));
    CHECK(panel->isVisible());
    QQuickItem* problem = named(root, QStringLiteral("nameProblem"));
    CHECK(problem->isVisible() &&
          problem->property("text").toString().contains(QStringLiteral("already named")));
    input->setProperty("text", QStringLiteral("Bass DI"));
    click(view, must(root, QStringLiteral("nameOk")));
    CHECK(!panel->isVisible());
    CHECK(session.trackNames().at(1) == QStringLiteral("Bass DI"));

    // A click on the colour bar opens the palette on the track, its colour
    // ringed; a swatch colours it and closes the palette.
    click(view, bar());
    QQuickItem* palette = named(root, QStringLiteral("colourPalette"));
    CHECK(palette != nullptr && QTest::qWaitFor([&]() { return palette->isVisible(); }));
    CHECK(palette->property("track").toInt() == 1 && palette->property("current").toInt() == 1);
    CHECK(!must(root, QStringLiteral("colourFollowTrack"))->isVisible());
    click(view, must(root, QStringLiteral("colourSwatch13")));
    CHECK(!palette->isVisible());
    CHECK(session.trackColours().at(1) == 13);
    CHECK(QTest::qWaitFor([&]() { return bar_is(13); }));

    // The Track menu carries both.
    session.selectTrack(2);
    QMetaObject::invokeMethod(named(root, QStringLiteral("trackMenu")), "show", Q_ARG(QVariant, 100),
                              Q_ARG(QVariant, 100));
    click(view, menu_item(root, QStringLiteral("trackColourItem")));
    CHECK(QTest::qWaitFor([&]() { return palette->isVisible(); }));
    CHECK(palette->property("track").toInt() == 2);
    // Escape closes it, as it does the other dialogs.
    QTest::keyClick(&view, Qt::Key_Escape);
    CHECK(!palette->isVisible());
    CHECK(model.invokeWith(QStringLiteral("track.colour"), 2, QString()));
    CHECK(QTest::qWaitFor([&]() { return palette->isVisible(); }));
    click(view, must(root, QStringLiteral("paletteClose")));
    CHECK(!palette->isVisible());

    // A region: Grabber-click it, then the palette offers its track's
    // colour back; the Edit menu's Rename Region… names it.
    session.controller().select_region(first_region(doc, 0).id);
    session.selectTrack(0);
    session.controller().select_region(first_region(doc, 0).id);
    CHECK(model.invokeWith(QStringLiteral("region.colour"), -1, QString()) == true);
    CHECK(QTest::qWaitFor([&]() { return palette->isVisible(); }));
    CHECK(palette->property("track").toInt() == -1 && palette->property("current").toInt() == -1);
    QQuickItem* follow = named(root, QStringLiteral("colourFollowTrack"));
    CHECK(follow->isVisible() && follow->property("active").toBool());
    click(view, must(root, QStringLiteral("colourSwatch4")));
    CHECK(!palette->isVisible() && first_region(doc, 0).colour == 4);
    CHECK(model.invokeWith(QStringLiteral("region.colour"), -1, QString()));
    CHECK(QTest::qWaitFor([&]() { return palette->isVisible(); }));
    click(view, follow);
    CHECK(!palette->isVisible() && first_region(doc, 0).colour == kTrackColour);
    CHECK(model.invokeWith(QStringLiteral("region.rename"), -1, QString()));
    CHECK(QTest::qWaitFor([&]() { return panel->isVisible(); }));
    CHECK(must(root, QStringLiteral("nameTitle"))->property("text").toString() ==
          QStringLiteral("Rename Region"));
    input->setProperty("text", QStringLiteral("Intro"));
    click(view, must(root, QStringLiteral("nameOk")));
    CHECK(!panel->isVisible() && first_region(doc, 0).name == "Intro");

    // A double-click on a region with the Grabber asks its name too.
    QQuickItem* timeline = named(root, QStringLiteral("timeline"));
    CHECK(timeline != nullptr);
    const ViewState& v = session.controller().view();
    const Region& r = first_region(doc, 0);
    const QPointF in_region(v.x_of(r.position + r.length / 2), v.y_of_track(0) + v.track_height * 0.75);
    session.controller().clear_selection();
    QTest::mouseDClick(&view, Qt::LeftButton, Qt::NoModifier, timeline->mapToScene(in_region).toPoint());
    CHECK(QTest::qWaitFor([&]() { return panel->isVisible(); }));
    CHECK(input->property("text").toString() == QStringLiteral("Intro"));
    click(view, must(root, QStringLiteral("nameCancel")));
    CHECK(!panel->isVisible() && first_region(doc, 0).name == "Intro");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_controller()) return 1;
    if (test_commands()) return 1;
    if (test_window()) return 1;
    std::printf("ui names and colours: ok\n");
    return 0;
}
