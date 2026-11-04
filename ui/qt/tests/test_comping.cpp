// SPDX-License-Identifier: GPL-3.0-or-later
//
// Comping in the Edit window (ADR-0031, ADR-0049): a track's other playlists
// as take lanes, a range dragged across a lane and copied to the main
// playlist (Pro Tools) or swiped at once (Logic's Quick Swipe), a new comp,
// the request Consolidate sends and its held notices. Then the exit test
// (ui.comping.exit_test): a Pro Tools engineer comps a vocal with the real
// window, using only what Pro Tools taught them -- the playlist view, a range
// on a lane, Copy Selection to Main Playlist, Undo -- and nothing from the
// documentation.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "comping_demo.h"
#include "edit_controller.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/consolidate_argument.h"
#include "sonora/api/fake_engine.h"
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
constexpr Frames kBar = 2 * kRate;   // 120 bpm, 4/4: the demo's bar
constexpr int kVox = 1;              // the track with the takes

const TrackEdit& track(const TimelineDocument& d, int row) {
    return *d.snapshot().edit->tracks[static_cast<std::size_t>(row)];
}

const Playlist* active(const TimelineDocument& d, int row) {
    return audio_playlist(track(d, row), track(d, row).active_playlist);
}

const Playlist* named_playlist(const TimelineDocument& d, int row, const std::string& name) {
    for (const auto& p : track(d, row).playlists) {
        if (p->name == name) return p.get();
    }
    return nullptr;
}

// Audio 2 with take 1 on its main playlist (bars 0 to 8) and takes 2 and 3
// on lanes of their own: what a three-pass loop take leaves, hand-built.
struct Fixture {
    ModelDocument doc{make_empty_session(2, kRate)};
    EditController c{doc};
    Id source1 = 0;

    bool build(std::string& why) {
        AddSource src;
        src.source.id = doc.reserve_id();
        src.source.path = "Audio Files/Audio 2_01.wav";
        src.source.length = 40 * kBar;
        src.source.take = 1;
        src.source.recorded_on_track = track(doc, kVox).id;
        source1 = src.source.id;
        if (!doc.submit(src, why)) return false;
        PlaceRegion place;
        place.playlist = track(doc, kVox).active_playlist;
        place.region.id = doc.reserve_id();
        place.region.name = "Audio 2 take 1";
        place.region.position = 0;
        place.region.length = 8 * kBar;
        place.region.channels = {{source1, 0}};
        if (!doc.submit(place, why)) return false;
        if (!add_demo_takes(doc, kVox, 0, 8, why)) return false;
        c.set_viewport(1000, 900);
        return true;
    }
    double x(Frames f) const { return c.view().x_of(f); }
    double lane_y(int lane) const { return c.view().y_of_complane(kVox, lane) + 10.0; }
    // A drag across take lane `lane` from `a` to `b`, as the mouse makes it.
    bool swipe(int lane, Frames a, Frames b, std::string& why) {
        const double y = lane_y(lane);
        if (!c.press(x(a), y, why)) return false;
        c.move(x(a) + 10, y);
        c.move(x(b), y);
        return c.release(x(b), y, why);
    }
};

// --- take lanes ---------------------------------------------------------------

int test_lanes() {
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    EditController& c = f.c;

    // Nothing shown until asked, and the rows are as they were.
    CHECK(!c.comp_lanes_shown(kVox));
    CHECK(c.take_lane_names(kVox) == (std::vector<std::string>{"Audio 2 take 2", "Audio 2 take 3"}));
    const double plain = c.view().content_height(2);
    CHECK(c.view().lanes_shown(kVox) == 0);

    CHECK(c.toggle_comp_lanes(kVox, why));
    CHECK(c.comp_lanes_shown(kVox) && c.view().lanes_shown(kVox) == 2);
    CHECK(c.view().content_height(2) == plain + 2 * c.view().comp_lane_height);
    // Only the track with takes grows; the one below keeps its place.
    CHECK(c.view().y_of_track(0) == 0);

    // A track with one playlist has no takes to show, and the refusal says
    // what makes some.
    CHECK(!c.toggle_comp_lanes(0, why));
    CHECK(why.find("one playlist") != std::string::npos && why.find("New Playlist") != std::string::npos);
    CHECK(!c.comp_lanes_shown(0));

    // The hit test: the row, then each lane, with the region under the pointer.
    const Hit row = c.hit(f.x(kBar), c.view().y_of_track(kVox) + 10);
    CHECK(row.zone != HitZone::TakeLane && row.track == kVox);
    const Hit l0 = c.hit(f.x(kBar), f.lane_y(0));
    CHECK(l0.zone == HitZone::TakeLane && l0.track == kVox && l0.lane == 0 && l0.region != kNoId);
    const Hit l1 = c.hit(f.x(kBar), f.lane_y(1));
    CHECK(l1.zone == HitZone::TakeLane && l1.lane == 1 && l1.region != l0.region);
    CHECK(c.hit(f.x(9 * kBar), f.lane_y(0)).region == kNoId);   // past the take: empty lane

    // Hiding them gives the rows back.
    CHECK(c.toggle_comp_lanes(kVox, why));
    CHECK(c.view().content_height(2) == plain && c.view().lanes_shown(kVox) == 0);

    // A new session forgets which tracks showed theirs, as automation lanes do.
    CHECK(c.toggle_comp_lanes(kVox, why));
    c.document_replaced();
    CHECK(!c.comp_lanes_shown(kVox));
    return 0;
}

// --- a swipe, by hand ---------------------------------------------------------

int test_swipe() {
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    EditController& c = f.c;
    CHECK(c.toggle_comp_lanes(kVox, why));
    const uint64_t rev = f.doc.snapshot().revision;

    // Dragging a range on a lane changes nothing yet: it is a selection of
    // the lane, apart from the one every other edit acts on.
    CHECK(f.swipe(0, kBar, 3 * kBar, why));
    CHECK(f.doc.snapshot().revision == rev);
    const auto ls = c.lane_selection();
    CHECK(ls.valid() && ls.track == kVox && ls.start == kBar && ls.end == 3 * kBar);
    CHECK(!c.selection().has_range());
    // A Clear now reaches neither the lane nor the comp through it.
    CHECK(!c.clear(why) || f.doc.snapshot().revision == rev);
    CHECK(c.lane_selection().valid());

    // Copy to Main Playlist: one step, the comp plays take 2 over the range
    // and take 1 around it.
    CHECK(c.comp_copy_to_main(why));
    CHECK(why.empty());
    CHECK(f.doc.undo_label() == "Copy to Main Playlist");
    const Playlist* main = active(f.doc, kVox);
    CHECK(main != nullptr && main->regions.size() == 3);
    CHECK(main->regions[0].end() == kBar && main->regions[1].position == kBar &&
          main->regions[1].end() == 3 * kBar && main->regions[2].position == 3 * kBar);
    CHECK(main->regions[1].channels[0].source != f.source1);
    CHECK(main->regions[0].channels[0].source == f.source1);
    CHECK(main->regions[2].channels[0].source == f.source1);
    // Which lane each piece came from is computed, never stored.
    CHECK(c.comp_origin_lane(kVox, main->regions[1]) == 0);
    CHECK(c.comp_origin_lane(kVox, main->regions[0]) == -1);
    // The swiped range is the selection now, so it can be auditioned.
    CHECK(c.selection().has_range() && c.selection().start == kBar && !c.lane_selection().valid());

    // Undo gives the take back whole.
    CHECK(c.undo(why));
    CHECK(active(f.doc, kVox)->regions.size() == 1);

    // A click on a lane's region picks it whole; a click on nothing picks nothing.
    CHECK(c.press(f.x(2 * kBar), f.lane_y(1), why) && c.release(f.x(2 * kBar), f.lane_y(1), why));
    CHECK(c.lane_selection().valid() && c.lane_selection().start == 0 &&
          c.lane_selection().end == 8 * kBar);
    CHECK(c.press(f.x(9 * kBar), f.lane_y(1), why) && c.release(f.x(9 * kBar), f.lane_y(1), why));
    CHECK(!c.lane_selection().valid());

    // With nothing selected on a lane the copy is refused and says what to do.
    CHECK(!c.comp_copy_to_main(why));
    CHECK(why.find("select a range on a take lane") != std::string::npos);
    CHECK(!c.select_lane_range(kVox, c.take_lane_ids(kVox)[0], kBar, kBar, why));
    CHECK(why.find("empty") != std::string::npos);

    // Pressing anywhere else drops the lane selection.
    CHECK(f.swipe(0, kBar, 2 * kBar, why) && c.lane_selection().valid());
    CHECK(c.press(f.x(kBar), c.view().y_of_track(kVox) + 10, why));
    CHECK(!c.lane_selection().valid());
    c.release(f.x(kBar), c.view().y_of_track(kVox) + 10, why);
    return 0;
}

// --- Quick Swipe, a new comp ----------------------------------------------------

int test_quick_swipe_and_new_comp() {
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    EditController& c = f.c;
    CHECK(c.toggle_comp_lanes(kVox, why));
    CHECK(!c.quick_swipe());
    c.set_quick_swipe(true);

    // The button coming up does the copy; the joins get a short crossfade.
    CHECK(f.swipe(1, 2 * kBar, 4 * kBar, why));
    CHECK(f.doc.undo_label() == "Copy to Main Playlist");
    const Playlist* main = active(f.doc, kVox);
    CHECK(main->regions.size() == 3 && main->regions[1].position == 2 * kBar);
    CHECK(c.comp_origin_lane(kVox, main->regions[1]) == 1);
    CHECK(main->crossfades.size() == 2);   // both joins had handles to draw on
    CHECK(why.empty());

    // A comp of its own: a new empty playlist, the one that was active is a
    // take lane now, the lanes open, and it is one undo step.
    CHECK(c.comp_new(kVox, why));
    CHECK(f.doc.undo_label() == "New Comp");
    CHECK(c.active_playlist_name(kVox) == "Audio 2.01 comp");
    CHECK(active(f.doc, kVox)->regions.empty());
    CHECK(c.take_lane_ids(kVox).size() == 3 && c.view().lanes_shown(kVox) == 3);
    // Swiping the earlier comp into it works like any take.
    const Id earlier = named_playlist(f.doc, kVox, "Audio 2.01")->id;
    CHECK(c.select_lane_range(kVox, earlier, 0, 8 * kBar, why));
    CHECK(c.comp_copy_to_main(why));
    CHECK(active(f.doc, kVox)->regions.size() == 3);
    CHECK(c.undo(why) && c.undo(why));
    CHECK(c.active_playlist_name(kVox) == "Audio 2.01");

    // The name is not taken twice.
    CHECK(c.comp_new(kVox, why));
    CHECK(c.choose_playlist(kVox, "Audio 2.01", why));
    CHECK(c.comp_new(kVox, why));
    CHECK(c.active_playlist_name(kVox) == "Audio 2.01 comp 2");
    return 0;
}

// --- Consolidate's request --------------------------------------------------------

int test_consolidate_request() {
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    EditController& c = f.c;
    EditController::ConsolidateRequest r;

    c.select_track(kVox);
    c.clear_selection();
    CHECK(!c.consolidate_request(r, why));
    CHECK(why.find("select a region") != std::string::npos);

    c.select_range(kVox, kBar, 3 * kBar);
    CHECK(c.consolidate_request(r, why));
    CHECK(r.track == track(f.doc, kVox).id && r.playlist == track(f.doc, kVox).active_playlist &&
          r.start == kBar && r.end == 3 * kBar);

    CHECK(c.select_region(active(f.doc, kVox)->regions[0].id));
    CHECK(c.consolidate_request(r, why));
    CHECK(r.start == 0 && r.end == 8 * kBar && r.name == "Audio 2 take 1");

    c.select_range(0, kBar, 3 * kBar, 2);
    CHECK(!c.consolidate_request(r, why));
    CHECK(why.find("one track") != std::string::npos || why.find("several") != std::string::npos);
    return 0;
}

// --- the scene --------------------------------------------------------------------

int test_scene() {
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    EditController& c = f.c;
    SyntheticPeakSource peaks(kRate, 120.0);
    Scene scene;
    const auto build = [&]() {
        build_timeline_scene(f.doc.snapshot(), c.view(), c.selection(), c.drag(), false,
                             GridValue::Beat, peaks, scene, c.lane_selection());
    };
    const auto labelled = [&](const std::string& text) {
        for (const SceneLabel& l : scene.labels) {
            if (l.text.find(text) != std::string::npos) return true;
        }
        return false;
    };
    const auto colour_at = [&](Frames frame, double y) {
        const SceneRect* r = rect_at(scene.rects, static_cast<float>(f.x(frame)), static_cast<float>(y));
        return r == nullptr ? 0u : r->argb;
    };
    constexpr uint32_t kSwipe = 0x553fc1b0;   // the drag in the hand, over its lane
    constexpr uint32_t kInUse = 0x3d3fc1b0;   // what the comp keeps, on the lane

    build();
    CHECK(!labelled("take 2"));   // lanes hidden: only the main region is drawn
    CHECK(c.toggle_comp_lanes(kVox, why));
    build();
    CHECK(labelled("Audio 2 take 2") && labelled("Audio 2 take 3"));

    // The drag in the hand is drawn on its lane before the button comes up.
    const double y0 = f.lane_y(0);
    CHECK(c.press(f.x(kBar), y0, why));
    c.move(f.x(kBar) + 20, y0);
    c.move(f.x(3 * kBar), y0);
    build();
    CHECK(c.drag().kind == Drag::Kind::Swipe);
    CHECK(colour_at(2 * kBar, y0 - 5) == kSwipe || colour_at(2 * kBar, f.lane_y(0) + 20) == kSwipe);
    CHECK(c.release(sx(3 * kBar), y0, why));
    build();
    CHECK(c.lane_selection().valid());

    // Copied: the lane shows what the comp keeps, and the comp's piece says
    // which take it came from.
    CHECK(c.comp_copy_to_main(why));
    build();
    CHECK(labelled("\xE2\x86\x90 Audio 2 take 2"));
    bool tinted = false;
    for (const SceneRect& r : scene.rects) tinted = tinted || r.argb == kInUse;
    CHECK(tinted);
    return 0;
}

// --- the commands and Consolidate's notices -----------------------------------------

int test_commands() {
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(f.doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    session.controller().set_viewport(1000, 900);
    const auto sx = [&session](Frames f) { return session.controller().view().x_of(f); };
    const auto sy = [&session](int lane) {
        return session.controller().view().y_of_complane(kVox, lane) + 10.0;
    };
    std::vector<std::pair<std::string, std::string>> sent;
    session.setCommandSender([&sent](const QString& command, const QString& argument) {
        sent.emplace_back(command.toStdString(), argument.toStdString());
    });

    CommandContext on;
    on.target = kVox;
    // Showing the lanes: the row says how many takes and whether they show.
    CHECK(session.trackRows().at(kVox).toMap().value("takes").toInt() == 2);
    CHECK(!session.trackRows().at(kVox).toMap().value("lanesShown").toBool());
    CHECK(local.dispatch("comp.toggle_lanes", on) == DispatchResult::Ok);
    CHECK(session.trackRows().at(kVox).toMap().value("lanesShown").toBool());
    on.target = 0;
    CHECK(local.dispatch("comp.toggle_lanes", on, why) == DispatchResult::Failed);
    CHECK(session.trackRefusal() == QString::fromStdString(why));   // held, not the status line
    session.dismissTrackRefusal();

    // Copy with no lane selection: refused and held.
    CHECK(local.dispatch("comp.copy_to_main", CommandContext{}, why) == DispatchResult::Failed);
    CHECK(session.trackRefusal().contains(QStringLiteral("select a range on a take lane")));

    // A drag on a lane, then the command, as the button and the key send it.
    session.press(sx(kBar), sy(0));
    session.drag(sx(2 * kBar), sy(0));
    session.release(sx(2 * kBar), sy(0));
    CHECK(session.laneSelection().value("ready").toBool() &&
          session.laneSelection().value("lane").toInt() == 0 &&
          session.laneSelection().value("track").toInt() == kVox);
    CHECK(local.dispatch("comp.copy_to_main", CommandContext{}) == DispatchResult::Ok);
    CHECK(session.trackRefusal().isEmpty());
    CHECK(!session.laneSelection().value("ready").toBool());
    CHECK(session.undoText() == QStringLiteral("Undo Copy to Main Playlist"));

    // Quick Swipe toggles; New comp is an undoable step.
    CHECK(!session.quickSwipe());
    CHECK(local.dispatch("comp.toggle_quick_swipe", CommandContext{}) == DispatchResult::Ok);
    CHECK(session.quickSwipe());
    on.target = kVox;
    CHECK(local.dispatch("comp.new", on) == DispatchResult::Ok);
    CHECK(session.undoText() == QStringLiteral("Undo New Comp"));

    // Consolidate sends the engine the selection; nothing selected is refused here.
    CHECK(local.dispatch("edit.consolidate", CommandContext{}, why) == DispatchResult::Failed);
    CHECK(why.find("select a region") != std::string::npos && sent.empty());
    session.controller().select_range(kVox, kBar, 3 * kBar);
    CHECK(local.dispatch("edit.consolidate", CommandContext{}) == DispatchResult::Ok);
    CHECK(sent.size() == 1 && sent[0].first == "session.consolidate");
    api::ConsolidateArgument arg;
    CHECK(api::parse_consolidate_argument(sent[0].second, arg, why));
    CHECK(arg.track == track(f.doc, kVox).id && arg.start == kBar && arg.end == 3 * kBar &&
          arg.playlist == track(f.doc, kVox).active_playlist);
    return 0;
}

// --- the real window: the exit test -------------------------------------------------

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

// ui.comping.exit_test. A Pro Tools engineer has three passes of a vocal
// phrase, take 1 on the track and takes 2 and 3 as its other playlists. They
// build the comp the way they always have -- show the playlists under the
// track, drag a range on the take they like, Copy Selection to Main Playlist,
// listen, undo what is wrong -- using the buttons on screen and the keys of
// their own profile, and consolidate the result. Every step is something the
// window offers where they look for it; nothing here needs the documentation.
int test_exit() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(8, api::testing::FakeEngine::engine_commands());
    Fixture f;
    std::string why;
    CHECK(f.build(why));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(f.doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    session.controller().set_viewport(1000, 900);
    const auto sx = [&session](Frames f) { return session.controller().view().x_of(f); };
    const auto sy = [&session](int lane) {
        return session.controller().view().y_of_complane(kVox, lane) + 10.0;
    };
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    session.setCommandSender([&model](const QString& command, const QString& argument) {
        model.invokeWith(command, -1, argument);
    });

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
    EditController& c = session.controller();

    // A key, as the profile has it: the chord resolved, then routed.
    const auto key = [&](const char* spec) {
        KeyChord chord;
        std::string e;
        if (!KeyMap::parse_chord(spec, chord, e)) return RouteResult{};
        return router.route(chord);
    };

    // 1. The track says it has two takes; a click on that shows them as lanes.
    QQuickItem* takes = nullptr;
    CHECK(QTest::qWaitFor([&]() {
        takes = named(root, QStringLiteral("takes1"));
        return takes != nullptr && takes->isVisible();
    }));
    CHECK(takes->property("text").toString().endsWith(QStringLiteral("2")));
    CHECK(named(root, QStringLiteral("takes0")) == nullptr ||
          !named(root, QStringLiteral("takes0"))->isVisible());   // a track without takes has no button
    click(view, takes);
    CHECK(c.comp_lanes_shown(kVox));
    QQuickItem* header0 = nullptr;
    CHECK(QTest::qWaitFor([&]() {
        header0 = named(root, QStringLiteral("takeHeader1_0"));
        return header0 != nullptr && header0->isVisible();
    }));
    CHECK(named(root, QStringLiteral("takeHeader1_1")) != nullptr);

    // 2. Drag across the part of take 2 that is good. Nothing is copied yet;
    // the button on the lane is lit, saying where the range will go.
    QQuickItem* toMain = named(root, QStringLiteral("toMain1_0"));
    CHECK(toMain != nullptr);
    CHECK(!toMain->property("active").toBool());
    session.press(sx(kBar), sy(0));
    session.drag(sx(2 * kBar), sy(0));
    session.drag(sx(3 * kBar), sy(0));
    session.release(sx(3 * kBar), sy(0));
    CHECK(QTest::qWaitFor([&]() { return toMain->property("active").toBool(); }));
    CHECK(active(f.doc, kVox)->regions.size() == 1);

    // 3. The button: Copy Selection to Main Playlist.
    click(view, toMain);
    CHECK(active(f.doc, kVox)->regions.size() == 3);
    CHECK(!toMain->property("active").toBool());

    // 4. The next phrase from take 3, with the profile's key this time.
    session.press(sx(3 * kBar), sy(1));
    session.drag(sx(4 * kBar), sy(1));
    session.drag(sx(6 * kBar), sy(1));
    session.release(sx(6 * kBar), sy(1));
    const RouteResult copied = key("ctrl+alt+c");
    CHECK(copied.kind == RouteResult::Kind::Local && copied.command == "comp.copy_to_main" &&
          copied.result == DispatchResult::Ok);
    const Playlist* main = active(f.doc, kVox);
    CHECK(main->regions.size() == 4);
    CHECK(c.comp_origin_lane(kVox, main->regions[1]) == 0);
    CHECK(c.comp_origin_lane(kVox, main->regions[2]) == 1);

    // 5. Listening, the second phrase was wrong: Undo takes it away, and only it.
    const RouteResult undone = key("ctrl+z");
    CHECK(undone.kind == RouteResult::Kind::Local && undone.result == DispatchResult::Ok);
    CHECK(active(f.doc, kVox)->regions.size() == 3);

    // 6. A button with nothing selected on its lane refuses and says why,
    // held where it can be read; it does not do nothing.
    const RouteResult nothing = key("ctrl+alt+c");
    CHECK(nothing.result == DispatchResult::Failed);
    CHECK(QTest::qWaitFor([&]() {
        QQuickItem* n = named(root, QStringLiteral("trackRefusal"));
        return n != nullptr && n->isVisible();
    }));
    CHECK(session.trackRefusal().contains(QStringLiteral("select a range on a take lane")));

    // 7. Consolidate the comped stretch: a range on the track, the profile's
    // key; the engine is sent the request and the window holds its answer.
    c.select_range(kVox, 0, 8 * kBar);
    const RouteResult cns = key("alt+shift+3");
    CHECK(cns.kind == RouteResult::Kind::Local && cns.result == DispatchResult::Ok);
    CHECK(!engine.sent.empty() && engine.sent.back().command == "session.consolidate");
    api::ConsolidateArgument arg;
    CHECK(api::parse_consolidate_argument(engine.sent.back().argument, arg, why));
    CHECK(arg.start == 0 && arg.end == 8 * kBar && arg.track == track(f.doc, kVox).id);

    EngineEvent running;
    running.kind = EngineEventKind::Consolidate;
    running.take = 420;
    running.flags = api::kConsolidateRunning;
    engine.events.push_back(running);
    model.tick();
    CHECK(model.consolidateRunning() && model.consolidateText().contains(QStringLiteral("42 %")));

    EngineEvent failed;
    failed.kind = EngineEventKind::Consolidate;
    failed.flags = api::kConsolidateFailed;
    failed.text = "the disk is full";
    engine.events.push_back(failed);
    model.tick();
    CHECK(model.consolidateFailed() && !model.consolidateRunning());
    CHECK(model.consolidateText().contains(QStringLiteral("the disk is full")));
    QQuickItem* notice = named(root, QStringLiteral("consolidateNotice"));
    CHECK(notice != nullptr && QTest::qWaitFor([&]() { return notice->isVisible(); }));
    click(view, named(root, QStringLiteral("dismissConsolidate")));
    CHECK(model.consolidateText().isEmpty());

    // A refusal the engine answers in its reply is held the same way.
    engine.next_result = DispatchResult::Failed;
    engine.next_message = "stop recording before consolidating";
    CHECK(key("alt+shift+3").result == DispatchResult::Ok);
    model.tick();
    CHECK(model.consolidateFailed() &&
          model.consolidateText().contains(QStringLiteral("stop recording before consolidating")));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_lanes() || test_swipe() || test_quick_swipe_and_new_comp() ||
        test_consolidate_request() || test_scene() || test_commands() || test_exit()) {
        return 1;
    }
    std::printf("ui comping: ok\n");
    return 0;
}
