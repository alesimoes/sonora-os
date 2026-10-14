// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tempo map as the counters, the ruler, the grid and snapping read it;
// the Transport window's model on a scripted transport; and the window in
// the real QML, opened by Pro Tools' Window > Transport key and closed by
// its button, on the offscreen platform.
//
// The regression this guards: bars and beats were read from the map's
// first entry alone, so a loaded session with a tempo change showed a
// counter, a ruler and a grid that disagreed with the session after the
// change, with nothing on screen to say so.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <cmath>
#include <cstdio>

#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/command.h"
#include "time_format.h"
#include "timeline_document.h"
#include "timeline_scene.h"
#include "transport_model.h"

using namespace sonora;
using namespace sonora::ui;
using model::Frames;
using model::SessionSettings;
using model::TempoEntry;

namespace {

int failures = 0;

#define OK(cond)                                                                    \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

constexpr Frames kRate = 48000;
constexpr Frames kBeat120 = 24000;      // a quarter at 120 bpm
constexpr Frames kBar120 = 96000;       // 4/4 at 120
constexpr Frames kBeat96 = 30000;       // a quarter at 96 bpm
constexpr Frames kBar96 = 120000;       // 4/4 at 96
constexpr Frames kAt96 = 16 * kBar120;              // bar 17
constexpr Frames kAt34 = kAt96 + 8 * kBar96;        // bar 25, 3/4 at 96

// The demo's map: 120 4/4, 96 4/4 from bar 17, 3/4 at 96 from bar 25.
SessionSettings mapped() {
    SessionSettings s;
    s.sample_rate = kRate;
    s.tempo = {TempoEntry{0, 120.0f, {{4, 4}}}, TempoEntry{kAt96, 96.0f, {{4, 4}}},
               TempoEntry{kAt34, 96.0f, {{3, 4}}}};
    return s;
}

std::string bb(Frames f, const SessionSettings& s) { return format_bars_beats(f, s); }

// --- reading positions -------------------------------------------------------

void test_reading() {
    // One entry reads as it always did.
    SessionSettings one;
    OK(bb(0, one) == "1|1|000");
    OK(bb(kBeat120, one) == "1|2|000");
    OK(bb(kBar120, one) == "2|1|000");
    OK(bb(kBeat120 / 2, one) == "1|1|480");
    OK(bb(-100, one) == "1|1|000");

    const SessionSettings s = mapped();
    // Before the first change, the first entry.
    OK(bb(kAt96 - kBeat120, s) == "16|4|000");
    // The change itself, and a quarter at the new tempo after it. Read from
    // the first entry, this was 17|2|240.
    OK(bb(kAt96, s) == "17|1|000");
    OK(bb(kAt96 + kBeat96, s) == "17|2|000");
    OK(bb(kAt96 + kBeat96 / 2, s) == "17|1|480");
    OK(bb(kAt34 - kBeat96, s) == "24|4|000");
    // The meter change: three beats to the bar.
    OK(bb(kAt34, s) == "25|1|000");
    OK(bb(kAt34 + 2 * kBeat96, s) == "25|3|000");
    OK(bb(kAt34 + 3 * kBeat96, s) == "26|1|000");

    OK(tempo_index_at(0, s) == 0);
    OK(tempo_index_at(kAt96 - 1, s) == 0);
    OK(tempo_index_at(kAt96, s) == 1);
    OK(tempo_index_at(kAt34 + 1, s) == 2);
    OK(tempo_at(kAt34, s).meter[0] == 3);
    OK(format_bpm(tempo_at(kAt96, s).bpm) == "96.00");
    OK(format_meter(tempo_at(kAt34, s)) == "3/4");

    // And back: the frame a bar|beat|tick falls on.
    OK(frame_of({17, 2, 0}, s) == kAt96 + kBeat96);
    OK(frame_of({26, 1, 0}, s) == kAt34 + 3 * kBeat96);
    OK(frame_of({1, 1, 0}, s) == 0);
    OK(frame_of({1, 1, 480}, s) == kBeat120 / 2);
    for (Frames f : {Frames{0}, kBeat120 * 7, kAt96, kAt96 + kBeat96 * 5, kAt34 + kBeat96 * 4}) {
        OK(frame_of(musical_time(f, s), s) == f);
    }

    // A tempo change inside a bar carries the count on: the bar straddles.
    SessionSettings mid;
    mid.sample_rate = kRate;
    mid.tempo = {TempoEntry{0, 120.0f, {{4, 4}}}, TempoEntry{6 * kBeat120, 60.0f, {{4, 4}}}};
    OK(bb(6 * kBeat120, mid) == "2|3|000");
    OK(bb(6 * kBeat120 + 48000, mid) == "2|4|000");
    OK(bb(6 * kBeat120 + 2 * 48000, mid) == "3|1|000");

    // A meter change inside a bar starts a bar there, and the bar it cut
    // short is still counted.
    SessionSettings cut;
    cut.sample_rate = kRate;
    cut.tempo = {TempoEntry{0, 120.0f, {{4, 4}}}, TempoEntry{2 * kBeat120, 120.0f, {{3, 4}}}};
    OK(bb(kBeat120, cut) == "1|2|000");
    OK(bb(2 * kBeat120, cut) == "2|1|000");
    OK(bb(5 * kBeat120, cut) == "3|1|000");
    // The beats the change cut off read as the change's own frame.
    OK(frame_of({1, 3, 0}, cut) == 2 * kBeat120);

    // A 6/8 bar: six eighths, each half a quarter.
    SessionSettings eight;
    eight.sample_rate = kRate;
    eight.tempo = {TempoEntry{0, 120.0f, {{6, 8}}}};
    OK(bb(6 * kBeat120 / 2, eight) == "2|1|000");
    OK(bb(kBeat120 / 2, eight) == "1|2|000");

    // A first entry not at 0 (the format forbids it) still counts from 0.
    SessionSettings late;
    late.sample_rate = kRate;
    late.tempo = {TempoEntry{1000, 120.0f, {{4, 4}}}};
    OK(bb(kBar120, late) == "2|1|000");
}

// --- grid, snapping, ruler ----------------------------------------------------

void test_grid() {
    const SessionSettings s = mapped();
    std::vector<GridLine> lines;
    grid_lines(0, kAt34 + 3 * kBeat96 + 10, GridValue::Bar, true, s, lines);
    // 16 bars at 120, 8 at 96, and two at 3/4.
    OK(lines.size() == 26);
    for (const GridLine& l : lines) OK(l.bar);
    if (lines.size() == 26) {
        OK(lines[15].frame == 15.0 * kBar120);
        OK(lines[16].frame == kAt96);
        OK(lines[17].frame == kAt96 + kBar96);
        OK(lines[24].frame == kAt34);
        OK(lines[25].frame == kAt34 + 3 * kBeat96);
    }
    // Beats in 3/4: the bar line, then two beats.
    grid_lines(kAt34, kAt34 + 3 * kBeat96 - 1, GridValue::Beat, false, s, lines);
    OK(lines.size() == 3);
    if (lines.size() == 3) {
        OK(lines[0].bar && !lines[1].bar && !lines[2].bar);
        OK(lines[2].frame == kAt34 + 2 * kBeat96);
    }
    // Sixteenths at 96: four to the beat.
    grid_lines(kAt96, kAt96 + kBeat96 - 1, GridValue::Sixteenth, false, s, lines);
    OK(lines.size() == 4);

    // Snapping finds the lines the map draws, not the first tempo's.
    OK(snap_to_grid(kAt96 + 3 * kBar96 + 1000, GridValue::Bar, s) == kAt96 + 3 * kBar96);
    OK(snap_to_grid(kAt34 + kBeat96 + 100, GridValue::Beat, s) == kAt34 + kBeat96);
    OK(snap_to_grid(kAt96 + kBeat96 / 2 + 10, GridValue::Beat, s) == kAt96 + kBeat96);
    // Across a change: the nearer line, whichever entry it belongs to.
    OK(snap_to_grid(kAt96 - 2000, GridValue::Bar, s) == kAt96);
    // The single-tempo cases hold.
    SessionSettings one;
    OK(snap_to_grid(kBeat120 + 100, GridValue::Beat, one) == kBeat120);
    OK(snap_to_grid(kBeat120 - 100, GridValue::Bar, one) == 0);
    OK(snap_to_grid(-5000, GridValue::Beat, one) == 0);
    OK(grid_frames(GridValue::Eighth, one) == kBeat120 / 2);

    // The ruler: labels at the map's bar lines, never crowded.
    std::vector<RulerTick> ticks;
    ruler_ticks(TimeFormat::BarsBeats, kAt34 - 96000, 500, 1000, s, ticks);
    bool saw25 = false, saw26 = false;
    for (const auto& t : ticks) {
        if (!t.major) continue;
        if (t.label == "25") saw25 = std::abs(t.x - 96000.0 / 500) < 0.5;
        if (t.label == "26") saw26 = std::abs(t.x - (96000.0 + 3 * kBeat96) / 500) < 0.5;
    }
    OK(saw25 && saw26);

    SessionSettings cut;   // a bar cut to two beats, the worst case for spacing
    cut.sample_rate = kRate;
    cut.tempo = {TempoEntry{0, 120.0f, {{4, 4}}}, TempoEntry{2 * kBeat120, 140.0f, {{7, 8}}},
                 TempoEntry{9 * kBeat120 + 77, 90.0f, {{4, 4}}}};
    for (const SessionSettings* m : {&s, static_cast<const SessionSettings*>(&cut)}) {
        for (double fpp : {20.0, 50.0, 300.0, 1200.0, 6000.0, 48000.0}) {
            for (double first : {0.0, 1e6, 2.4e6}) {
                ruler_ticks(TimeFormat::BarsBeats, first, fpp, 1000, *m, ticks);
                double last = -1e9, prev_x = -1e9;
                int majors = 0;
                for (const auto& t : ticks) {
                    OK(t.x >= prev_x - 1e-6);   // in order
                    prev_x = t.x;
                    if (!t.major) continue;
                    OK(t.x - last >= 89.0);
                    OK(!t.label.empty());
                    last = t.x;
                    ++majors;
                }
                // A view narrower than a beat may hold no label at all.
                if (fpp >= 50.0) OK(majors >= 1);
            }
        }
    }

    // The Edit window's grid lines are the map's.
    ViewState v;
    v.width = 1000;
    v.height = 200;
    v.fpp = 500;
    v.scroll = static_cast<double>(kAt34 - 96000);
    ModelDocument doc(make_empty_session(1, kRate));
    model::Snapshot snap = doc.snapshot();
    snap.settings = s;
    SyntheticPeakSource peaks(kRate, 120.0);
    Scene scene;
    build_timeline_scene(snap, v, Selection{}, Drag{}, true, GridValue::Bar, peaks, scene);
    const double bar25_x = std::floor((static_cast<double>(kAt34) - v.scroll) / v.fpp);
    bool line_at_25 = false;
    for (const auto& r : scene.rects) {
        if (r.w == 1 && r.h == static_cast<float>(v.height) && r.x == static_cast<float>(bar25_x)) {
            line_at_25 = true;
        }
    }
    OK(line_at_25);
}

// --- the Transport window's model ----------------------------------------------

void test_model() {
    SessionSettings settings = mapped();
    Frames insertion = kBeat120;
    TransportModel t([&settings]() -> const SessionSettings& { return settings; },
                     [&insertion]() { return insertion; });

    // Before any tick: the insertion point, at the first entry's tempo.
    OK(t.barsBeats() == QStringLiteral("1|2|000"));
    OK(t.tempoText() == QStringLiteral("120.00"));
    OK(t.meterText() == QStringLiteral("4/4"));
    OK(t.tempoChanges() == 2);
    OK(t.nextChangeText() == QStringLiteral("96.00  4/4 at 17|1|000"));
    OK(t.tempoMap().size() == 3);
    OK(!t.open());

    TransportSnapshot snap;
    t.onTransport(snap, true);
    OK(t.stateText() == QStringLiteral("STOP"));
    OK(t.positionSource() == QStringLiteral("insertion point"));

    // Rolling: the playhead, at once on the change of state.
    QSignalSpy positions(&t, &TransportModel::positionChanged);
    QSignalSpy tempos(&t, &TransportModel::tempoChanged);
    snap.state = TransportMode::Rolling;
    snap.playhead_frames = static_cast<uint64_t>(kAt96 + kBeat96);
    t.onTransport(snap, true);
    OK(t.stateText() == QStringLiteral("PLAY"));
    OK(t.rolling() && !t.recording());
    OK(t.positionSource() == QStringLiteral("playhead"));
    OK(t.barsBeats() == QStringLiteral("17|2|000"));
    OK(t.timecode() == QString::fromStdString(format_timecode(kAt96 + kBeat96, settings)));
    OK(t.samples() == QString::number(kAt96 + kBeat96));
    OK(t.tempoText() == QStringLiteral("96.00"));
    OK(t.nextChangeText() == QStringLiteral("96.00  3/4 at 25|1|000"));
    OK(t.tempoMap().at(1).toMap().value(QStringLiteral("current")).toBool());
    OK(!t.tempoMap().at(0).toMap().value(QStringLiteral("current")).toBool());
    OK(positions.count() == 1);
    OK(tempos.count() == 1);

    // While rolling, the text follows at a quarter of the ticks.
    positions.clear();
    for (int i = 1; i <= 8; ++i) {
        snap.playhead_frames += 512;
        t.onTransport(snap, true);
    }
    OK(positions.count() == 2);
    OK(t.position() == static_cast<Frames>(snap.playhead_frames));

    // Past the last change: no next change, the 3/4 entry current.
    snap.state = TransportMode::Recording;
    snap.playhead_frames = static_cast<uint64_t>(kAt34 + kBeat96);
    t.onTransport(snap, true);
    OK(t.stateText() == QStringLiteral("REC"));
    OK(t.recording());
    OK(t.meterText() == QStringLiteral("3/4"));
    OK(t.nextChangeText().isEmpty());
    OK(t.barsBeats() == QStringLiteral("25|2|000"));

    // A stale read is not a stop: what was shown stays.
    TransportSnapshot stale;
    stale.stale = true;
    t.onTransport(stale, true);
    OK(t.stateText() == QStringLiteral("REC"));
    OK(t.barsBeats() == QStringLiteral("25|2|000"));

    // A dead engine: not its last playhead, which looks current; ours.
    t.onTransport(TransportSnapshot{}, false);
    OK(t.stateText() == QStringLiteral("NO ENGINE"));
    OK(!t.alive() && !t.rolling());
    OK(t.barsBeats() == QStringLiteral("1|2|000"));
    OK(t.positionSource().contains(QStringLiteral("no engine")));

    // The session's settings change under it: noticed on the next tick.
    QSignalSpy settings_spy(&t, &TransportModel::settingsChanged);
    OK(!t.loopSet());
    OK(t.rangeText(t.loopStart(), t.loopEnd(), QStringLiteral("Bars|Beats")) ==
       QStringLiteral("not set"));
    settings.transport.loop = {{kAt96, kAt96 + 2 * kBar96}};
    settings.transport.pre_roll = kRate * 3 / 2;
    t.onTransport(TransportSnapshot{}, true);
    OK(settings_spy.count() == 1);
    OK(t.loopSet());
    OK(t.rangeText(t.loopStart(), t.loopEnd(), QStringLiteral("Bars|Beats")) ==
       QStringLiteral("17|1|000 – 19|1|000"));
    OK(t.timeText(kRate, QStringLiteral("Min:Secs")) == QStringLiteral("0:01.000"));
    OK(t.timeText(kRate, QStringLiteral("Feet+Frames")) == QStringLiteral("?"));
    OK(t.preRollText() == QStringLiteral("1.500 s"));
    tempos.clear();
    settings.tempo.pop_back();
    t.onTransport(TransportSnapshot{}, true);
    OK(tempos.count() == 1);
    OK(t.tempoChanges() == 1);

    // What the engine does not run is said, never offered.
    OK(!TransportModel::clickAvailable());
    OK(!TransportModel::rollsAvailable());
    OK(TransportModel::clickWhy().contains(QStringLiteral("No click")));

    // window.transport is the interface's, and toggles.
    CommandRegistry local;
    t.installCommands(local);
    OK(find_command("window.transport") != nullptr);
    OK(!is_engine_command("window.transport"));
    OK(local.dispatch("window.transport") == DispatchResult::Ok);
    OK(t.open());
    OK(local.dispatch("window.transport") == DispatchResult::Ok);
    OK(!t.open());
}

// --- the window, in the real QML ----------------------------------------------

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

void key(QQuickView& view, int k, Qt::KeyboardModifiers m) {
    QKeyEvent press(QEvent::KeyPress, k, m);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m);
    QCoreApplication::sendEvent(&view, &release);
}

void click(QQuickView& view, QQuickItem* item) {
    const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, c.toPoint());
}

void test_window() {
    KeyMap map;
    std::string err;
    OK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));

    test::RecordingAdapter engine(18, api::testing::FakeEngine::engine_commands());
    model::Snapshot demo = make_demo_session(18, kRate);
    demo.settings.tempo = mapped().tempo;
    ModelDocument document(demo);
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(document, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    TransportModel transport(
        [&session]() -> const SessionSettings& { return session.controller().snapshot().settings; },
        [&session]() { return session.controller().selection().insertion; });
    transport.installCommands(router.local());
    router.prepare(qt_device_chords(true));

    uint64_t now = 1;
    ShellModel model(engine, router);
    model.setClock([&now]() { return now; });
    model.setTransportSink([&](const TransportSnapshot& t, bool alive) {
        session.onTransport(t, alive);
        transport.onTransport(t, alive);
    });

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    OK(view.status() == QQuickView::Ready);
    if (view.status() != QQuickView::Ready) return;
    QQuickItem* root = view.rootObject();
    // Without a model there is no Transport window and no button for one.
    OK(named(root, QStringLiteral("transportWindow")) == nullptr);
    QQuickItem* button = named(root, QStringLiteral("transportButton"));
    OK(button != nullptr && !button->isVisible());

    root->setProperty("transport", QVariant::fromValue(&transport));
    view.show();
    model.routeKeysFrom(&view);
    model.setScanCodesAreXkb(true);
    model.tick();
    QQuickItem* win = named(root, QStringLiteral("transportWindow"));
    OK(win != nullptr);
    if (win == nullptr) return;
    OK(!win->isVisible());
    OK(button->isVisible());

    // Pro Tools' Window > Transport: Ctrl+1 on the keypad.
    key(view, Qt::Key_1, Qt::ControlModifier | Qt::KeypadModifier);
    OK(transport.open());
    OK(win->isVisible());
    // The main row's 1 is not the keypad's.
    key(view, Qt::Key_1, Qt::ControlModifier);
    OK(transport.open());

    // It reads what the model says, which reads the map.
    auto text = [&](const char* name) {
        QQuickItem* i = named(root, QString::fromLatin1(name));
        return i ? i->property("text").toString() : QStringLiteral("<missing>");
    };
    engine.snapshot.state = TransportMode::Rolling;
    engine.snapshot.playhead_frames = static_cast<uint64_t>(kAt96 + kBeat96);
    model.tick();
    OK(text("transportBarsBeats") == QStringLiteral("17|2|000"));
    // The big counter is the same reading, in the main counter's format.
    OK(text("transportCounter") == QStringLiteral("17|2|000"));
    session.cycleMainCounter();   // Min:Secs
    OK(text("transportCounter") == text("transportMinSecs"));
    for (int i = 0; i < 3; ++i) session.cycleMainCounter();   // back to Bars|Beats
    OK(text("transportTempo").contains(QStringLiteral("96.00")));
    OK(text("transportMeter") == QStringLiteral("4/4"));
    OK(text("barTempo") == QStringLiteral("96.00 bpm  4/4"));
    // The main counter (Bars|Beats by default) reads the map too, at its
    // own quarter rate; a frame or two past the beat is still its tick 0.
    for (int i = 1; i <= 4; ++i) {
        engine.snapshot.playhead_frames = static_cast<uint64_t>(kAt96 + kBeat96 + i);
        model.tick();
    }
    OK(session.mainCounterFormat() == QStringLiteral("Bars|Beats"));
    OK(session.mainCounter() == QStringLiteral("17|2|000"));

    // The click is not in this build: a press says so, nothing lights.
    QQuickItem* click_button = named(root, QStringLiteral("transportClick"));
    OK(click_button != nullptr);
    if (click_button != nullptr) {
        OK(!click_button->property("available").toBool());
        click(view, click_button);
        OK(model.statusText().contains(QStringLiteral("No click")));
    }

    // The button in the bar and the close box send the key's command.
    QQuickItem* close = named(root, QStringLiteral("transportClose"));
    OK(close != nullptr);
    if (close != nullptr) click(view, close);
    OK(!transport.open());
    OK(!win->isVisible());
    click(view, button);
    OK(transport.open());
    click(view, button);
    OK(!transport.open());
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    test_reading();
    test_grid();
    test_model();
    test_window();
    if (failures != 0) {
        std::fprintf(stderr, "test_ui_transport: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_ui_transport: ok\n");
    return 0;
}
