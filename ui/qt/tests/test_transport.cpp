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
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
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
#include "theme.h"
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
    // Also the last of them, and past its start: no beat of the bar the
    // change ended lands inside the new meter's first bar.
    OK(frame_of({1, 4, 480}, cut) == 2 * kBeat120);
    SessionSettings cut2 = cut;   // 2/4: the cut-off beats overlap the new bar's
    cut2.tempo[1].meter = {{2, 4}};
    OK(bb(2 * kBeat120, cut2) == "2|1|000");
    OK(frame_of({1, 4, 0}, cut2) == 2 * kBeat120);
    OK(frame_of({1, 3, 480}, cut2) == 2 * kBeat120);
    OK(frame_of({2, 2, 0}, cut2) == 3 * kBeat120);

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

    // A meter change just past a bar line leaves a sliver of a bar, far
    // closer than 4 px to the line before it at this zoom. That one line is
    // dropped; the rest of the grid stays.
    SessionSettings sliver;
    sliver.sample_rate = kRate;
    sliver.tempo = {TempoEntry{0, 120.0f, {{4, 4}}}, TempoEntry{kBar120 + 100, 120.0f, {{3, 4}}}};
    snap.settings = sliver;
    v.scroll = 0;
    auto lines_drawn = [&](bool grid_mode, GridValue g) {
        build_timeline_scene(snap, v, Selection{}, Drag{}, grid_mode, g, peaks, scene);
        std::vector<float> xs;
        for (const auto& r : scene.rects) {
            if (r.w == 1 && r.h == static_cast<float>(v.height)) xs.push_back(r.x);
        }
        return xs;
    };
    auto drawn_at = [](const std::vector<float>& xs, double frame, double fpp) {
        const auto x = static_cast<float>(std::floor(frame / fpp));
        return std::find(xs.begin(), xs.end(), x) != xs.end();
    };
    for (bool grid_mode : {false, true}) {
        const std::vector<float> xs = lines_drawn(grid_mode, GridValue::Bar);
        OK(drawn_at(xs, kBar120, v.fpp));                              // bar 2
        OK(drawn_at(xs, kBar120 + 100 + 3 * kBeat120, v.fpp));         // bar 4
        OK(drawn_at(xs, kBar120 + 100 + 6 * kBeat120, v.fpp));         // bar 5
        for (std::size_t i = 1; i < xs.size(); ++i) OK(xs[i] - xs[i - 1] >= 3.0f);
    }
    // A grid uniformly closer than 4 px is still not drawn at all.
    v.fpp = kBeat120 / 3.0;
    OK(lines_drawn(true, GridValue::Beat).empty());
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
    // The map changes and the position does not: every reading follows the
    // map at once, not when the position next moves.
    insertion = kAt96 + kBeat96;
    t.onTransport(TransportSnapshot{}, true);
    OK(t.barsBeats() == QStringLiteral("17|2|000"));
    settings.tempo.pop_back();   // 120 throughout: 16 bars and 1.25 beats
    t.onTransport(TransportSnapshot{}, true);
    OK(t.barsBeats() == QStringLiteral("17|2|240"));
    // The same for the sample rate, which Timecode and Min:Secs read.
    settings.sample_rate = 96000;
    t.onTransport(TransportSnapshot{}, true);
    OK(t.minSecs() == QString::fromStdString(format_min_sec(kAt96 + kBeat96, 96000)));
    OK(t.timecode() == QString::fromStdString(format_timecode(kAt96 + kBeat96, settings)));
    settings.sample_rate = kRate;
    settings.tempo = mapped().tempo;
    insertion = kBeat120;
    t.onTransport(TransportSnapshot{}, true);

    // The engine runs pre- and post-roll around a punch, a loop take and a
    // played selection (engine.transport.preroll), so the window no longer
    // dims them as unused; the line under them says where they apply.
    OK(TransportModel::rollsAvailable());
    OK(!TransportModel::rollsWhy().contains(QStringLiteral("does not run")));
    OK(TransportModel::rollsWhy().contains(QStringLiteral("plain take")));

    // A typed roll is seconds; anything else is refused with the reason.
    {
        Frames f = -1;
        std::string why;
        OK(parse_roll_text("2", kRate, f, why) && f == 2 * kRate);
        OK(parse_roll_text("0.5 s", kRate, f, why) && f == kRate / 2);
        OK(parse_roll_text("1.25S", kRate, f, why) && f == 60000);
        OK(parse_roll_text("0", kRate, f, why) && f == 0);
        OK(parse_roll_text("60", kRate, f, why) && f == 60 * kRate);
        f = 7;
        for (const char* bad : {"", "s", ".", "-1", "1.2.3", "two", "1:00", "61", "200"}) {
            why.clear();
            OK(!parse_roll_text(bad, kRate, f, why) && !why.empty() && f == 7);
        }
        OK(!parse_roll_text("2", 0, f, why));
    }
    // The click and count-off switches are the state block's, off with no
    // engine.
    TransportSnapshot lit;
    lit.click = true;
    lit.count_off = true;
    lit.counting_off = true;
    t.onTransport(lit, true);
    OK(t.clickOn() && t.countOffOn() && t.countingOff());
    t.onTransport(lit, false);
    OK(!t.clickOn() && !t.countOffOn() && !t.countingOff());
    t.onTransport(TransportSnapshot{}, true);
    OK(!t.clickOn() && !t.countOffOn());

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

// Types `text` into whatever has the keyboard, a key at a time.
void type(QQuickView& view, const char* text) {
    for (const char* c = text; *c != '\0'; ++c) QTest::keyClick(&view, *c);
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
    link_transport(model, session, transport);   // as main.cpp wires them
    link_marker_locate(model, session);

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
    // Past a punch's B + H the engine drops from Recording to Rolling: both
    // record buttons release, as Pro Tools' does at the punch-out, and the
    // transport shows a roll, not a take.
    {
        const auto active = [&](const char* name) {
            QQuickItem* i = named(root, QString::fromLatin1(name));
            return i != nullptr && i->property("active").toBool();
        };
        OK(named(root, QStringLiteral("barRecord")) != nullptr);
        OK(named(root, QStringLiteral("transportRecord")) != nullptr);
        engine.snapshot.state = TransportMode::Recording;
        engine.snapshot.playhead_frames = static_cast<uint64_t>(kAt96);
        // The bar reads the state block every fourth tick (kStateEvery).
        for (int i = 0; i < 4; ++i) model.tick();
        OK(model.recording() && transport.recording());
        OK(active("barRecord") && active("transportRecord"));
        OK(transport.stateText() == QStringLiteral("REC"));
        engine.snapshot.state = TransportMode::Rolling;
        for (int i = 0; i < 4; ++i) model.tick();
        OK(!model.recording() && !transport.recording() && transport.rolling());
        OK(!active("barRecord") && !active("transportRecord"));
        OK(transport.stateText() == QStringLiteral("PLAY"));
    }
    engine.snapshot.state = TransportMode::Rolling;
    engine.snapshot.playhead_frames = static_cast<uint64_t>(kAt96 + kBeat96);
    model.tick();
    // The window publishes on the main counter's cue, so from the first tick
    // of the roll the two agree, whichever position that cue has reached.
    OK(text("transportCounter") == session.mainCounter());
    // The main counter (Bars|Beats by default) reads the map too, at its
    // own quarter rate; a frame or two past the beat is still its tick 0.
    for (int i = 1; i <= 4; ++i) {
        engine.snapshot.playhead_frames = static_cast<uint64_t>(kAt96 + kBeat96 + i);
        model.tick();
        OK(text("transportCounter") == session.mainCounter());
    }
    OK(session.mainCounterFormat() == QStringLiteral("Bars|Beats"));
    OK(session.mainCounter() == QStringLiteral("17|2|000"));
    OK(text("transportBarsBeats") == QStringLiteral("17|2|000"));
    // The big counter is the same reading, in the main counter's format.
    OK(text("transportCounter") == QStringLiteral("17|2|000"));
    session.cycleMainCounter();   // Min:Secs
    OK(text("transportCounter") == text("transportMinSecs"));
    OK(text("transportCounter") == session.mainCounter());
    for (int i = 0; i < 3; ++i) session.cycleMainCounter();   // back to Bars|Beats
    OK(text("transportTempo").contains(QStringLiteral("96.00")));
    OK(text("transportMeter") == QStringLiteral("4/4"));
    OK(text("barTempo") == QStringLiteral("96.00 bpm  4/4"));
    // Rolling, the bar's counter and the window show one reading on every
    // tick, never two a few ticks apart.
    for (int i = 1; i <= 12; ++i) {
        engine.snapshot.playhead_frames += 700;
        model.tick();
        OK(session.mainCounter() == transport.barsBeats());
        OK(text("transportCounter") == session.mainCounter());
    }
    // A stale read is one tick's seqlock miss, not a stop (#62 merge gate):
    // the bar counter used to drop to the insertion point while the window
    // kept the playhead, and to stay there up to four ticks after the
    // reads came back.
    const QString before_stale = session.mainCounter();
    engine.snapshot.stale = true;
    for (int i = 0; i < 3; ++i) {
        model.tick();
        OK(session.transportOverlay().rolling);
        OK(session.mainCounter() == before_stale);
        OK(session.mainCounter() == transport.barsBeats());
    }
    engine.snapshot.stale = false;
    for (int i = 1; i <= 8; ++i) {
        engine.snapshot.playhead_frames += 700;
        model.tick();
        OK(session.mainCounter() == transport.barsBeats());
        OK(text("transportCounter") == session.mainCounter());
    }

    // Counter entry: a click on the digits opens the counter, Enter locates.
    {
        engine.snapshot.state = TransportMode::Stopped;
        for (int i = 0; i < 4; ++i) model.tick();
        OK(!transport.rolling());
        const auto last_locate = [&engine]() -> std::string {
            for (auto it = engine.sent.rbegin(); it != engine.sent.rend(); ++it) {
                if (it->command == "transport.locate") return it->argument;
            }
            return "none";
        };
        QSignalSpy refusals(&session, &SessionModel::refused);
        QQuickItem* counter = named(root, QStringLiteral("transportCounter"));
        QQuickItem* input = named(root, QStringLiteral("transportCounterInput"));
        OK(counter != nullptr && input != nullptr);
        if (counter != nullptr && input != nullptr) {
            click(view, counter);
            OK(counter->property("editing").toBool() && input->hasActiveFocus());
            OK(text("transportCounterCaption").contains(QStringLiteral("bar|beat|tick")));
            // Keys go to the field, not the profile: "2" picks no tool and
            // Return sends no command of its own.
            type(view, "25|3");
            OK(input->property("text").toString() == QStringLiteral("25|3"));
            key(view, Qt::Key_Return, Qt::NoModifier);
            OK(!counter->property("editing").toBool());
            OK(session.controller().selection().insertion == kAt34 + 2 * kBeat96);
            OK(last_locate() == std::to_string(kAt34 + 2 * kBeat96));
            OK(session.mainCounter() == QStringLiteral("25|3|000"));
            model.tick();   // the window reads the insertion point on the tick
            OK(text("transportCounter") == QStringLiteral("25|3|000"));
            OK(refusals.isEmpty());

            // Not a position: the field stays open, outlined, and says why;
            // nothing moves and nothing is sent.
            const std::size_t sent = engine.sent.size();
            click(view, counter);
            type(view, "25|4");
            key(view, Qt::Key_Return, Qt::NoModifier);
            OK(counter->property("editing").toBool());
            OK(counter->property("refused").toBool());
            // The reason is held beside the field, not only on the status line.
            OK(text("transportCounterCaption").contains(QStringLiteral("beats 1 to 3")));
            OK(refusals.size() == 1 &&
               refusals.last().at(0).toString().contains(QStringLiteral("beats 1 to 3")));
            OK(session.controller().selection().insertion == kAt34 + 2 * kBeat96);
            OK(engine.sent.size() == sent);
            // Escape leaves it where it was.
            key(view, Qt::Key_Escape, Qt::NoModifier);
            OK(!counter->property("editing").toBool());
            OK(session.controller().selection().insertion == kAt34 + 2 * kBeat96);
        }
        // In Timecode, all four fields.
        session.cycleMainCounter();   // Min:Secs
        session.cycleMainCounter();   // Timecode
        OK(session.enterCounter(QStringLiteral("01:00:02:00")));
        OK(session.controller().selection().insertion == 2 * kRate);
        OK(!session.enterCounter(QStringLiteral("01:00:02")));
        session.cycleMainCounter();   // Samples
        session.cycleMainCounter();   // back to Bars|Beats

        // During a take the engine refuses a locate, and the insertion point
        // must not move under a counter that shows the playhead.
        engine.snapshot.state = TransportMode::Recording;
        for (int i = 0; i < 4; ++i) model.tick();
        const Frames before = session.controller().selection().insertion;
        OK(!session.enterCounter(QStringLiteral("1")));
        OK(!refusals.isEmpty() &&
           refusals.last().at(0).toString().contains(QStringLiteral("take")));
        OK(session.controller().selection().insertion == before);
        engine.snapshot.state = TransportMode::Stopped;
        for (int i = 0; i < 4; ++i) model.tick();

        // The bar's counter is the same field; beside it, a click cycles.
        QQuickItem* bar_counter = named(root, QStringLiteral("mainCounter"));
        OK(bar_counter != nullptr);
        if (bar_counter != nullptr) {
            click(view, bar_counter);
            OK(bar_counter->property("editing").toBool());
            key(view, Qt::Key_Escape, Qt::NoModifier);
            OK(!bar_counter->property("editing").toBool());
        }
        QQuickItem* caption = named(root, QStringLiteral("mainCounterCaption"));
        OK(caption != nullptr);
        if (caption != nullptr) {
            click(view, caption);
            OK(session.mainCounterFormat() == QStringLiteral("Min:Secs"));
            for (int i = 0; i < 3; ++i) session.cycleMainCounter();
        }
    }

    // Click and Count-off send their keys' commands, and light as the
    // engine says, not on the press.
    for (const auto& b : {std::make_pair("transportClick", "transport.toggle_click"),
                          std::make_pair("transportCountOff", "transport.toggle_count_off")}) {
        QQuickItem* item = named(root, QString::fromLatin1(b.first));
        OK(item != nullptr);
        if (item == nullptr) continue;
        OK(item->property("available").toBool() && !item->property("active").toBool());
        click(view, item);
        OK(!engine.sent.empty() && engine.sent.back().command == b.second);
        OK(!item->property("active").toBool());
    }
    engine.snapshot.click = true;
    engine.snapshot.count_off = true;
    engine.snapshot.counting_off = true;
    for (int i = 0; i < 4; ++i) model.tick();
    QQuickItem* click_button = named(root, QStringLiteral("transportClick"));
    QQuickItem* count_off_button = named(root, QStringLiteral("transportCountOff"));
    OK(click_button != nullptr && count_off_button != nullptr);
    if (click_button != nullptr && count_off_button != nullptr) {
        OK(click_button->property("active").toBool());
        OK(count_off_button->property("active").toBool());
        OK(count_off_button->property("text").toString() == QStringLiteral("Counting"));
    }
    engine.snapshot.click = false;
    engine.snapshot.count_off = false;
    engine.snapshot.counting_off = false;
    for (int i = 0; i < 4; ++i) model.tick();

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

// A long tempo map (a tempo-mapped live take, a change every beat): the
// Transport window's list is read from one map, not a map per entry. Built
// per entry, 20,000 entries took tens of seconds; from one, milliseconds.
void test_long_map() {
    SessionSettings s;
    s.sample_rate = kRate;
    constexpr std::size_t kEntries = 20'000;
    s.tempo.clear();  // not the default entry: one map entry per beat
    s.tempo.reserve(kEntries);
    for (std::size_t i = 0; i < kEntries; ++i) {
        const auto at = static_cast<Frames>(i) * 24'000;
        s.tempo.push_back(TempoEntry{at, i % 2 == 0 ? 120.0f : 121.0f,
                                     {{i % 3 == 0 ? 3u : 4u, 4u}}});
    }
    Frames insertion = 0;
    QElapsedTimer clock;
    clock.start();
    TransportModel t([&s]() -> const SessionSettings& { return s; },
                     [&insertion]() { return insertion; });
    const qint64 ms = clock.elapsed();
    std::printf("test_ui_transport: %zu-entry tempo list (%lld listed) in %lld ms\n", kEntries,
                static_cast<long long>(t.tempoMap().size()), static_cast<long long>(ms));
    OK(ms < 2000);
    OK(t.tempoMap().size() == static_cast<int>(kEntries));
    // The same readings a position gets on its own.
    for (std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{7}, kEntries / 2,
                          kEntries - 1}) {
        OK(t.tempoMap().at(static_cast<int>(i)).toMap().value(QStringLiteral("at")).toString() ==
           QString::fromStdString(bb(i == 0 ? 0 : s.tempo[i].at, s)));
    }
    OK(t.nextChangeText().endsWith(QString::fromStdString(" at " + bb(s.tempo[1].at, s))));
}

// --- counter entry -------------------------------------------------------------

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void test_counter_parse() {
    const SessionSettings s = mapped();
    SessionSettings df = s;
    df.timecode.fps = model::Fps::F29_97df;
    df.timecode.origin = 0;
    Frames f = 0;
    std::string why;
    const auto parse = [&](TimeFormat fmt, const std::string& t, const SessionSettings& ss) {
        why.clear();
        f = -1;
        return parse_time(fmt, t, ss, f, why);
    };
    constexpr TimeFormat BB = TimeFormat::BarsBeats, TC = TimeFormat::Timecode,
                         MS = TimeFormat::MinSec, SA = TimeFormat::Samples;

    // Bars|Beats through the whole map, fields left out are the bar's start.
    OK(parse(BB, "17", s) && f == kAt96);
    OK(parse(BB, "17|2", s) && f == kAt96 + kBeat96);
    OK(parse(BB, "17|2|480", s) && f == kAt96 + kBeat96 + kBeat96 / 2);
    OK(parse(BB, " 1.1.000 ", s) && f == 0);   // the keypad's separator
    OK(parse(BB, "25 3", s) && f == kAt34 + 2 * kBeat96);
    // Bar 25 is in 3/4: there is no fourth beat to go to, and the next bar's
    // first is not what was typed.
    OK(!parse(BB, "25|4", s) && has(why, "beats 1 to 3"));
    OK(!parse(BB, "17|5", s) && has(why, "beats 1 to 4"));
    OK(!parse(BB, "0", s) && has(why, "no bar 0"));
    OK(!parse(BB, "1|1|960", s) && has(why, "ticks 0 to 959"));
    OK(!parse(BB, "1|1|1|1", s));
    OK(!parse(BB, "1||2", s));
    OK(!parse(BB, "verse", s));
    OK(!parse(BB, "", s) && why == "type a position first");
    OK(!parse(BB, "-3", s) && has(why, "negative"));
    OK(!parse(BB, "999999999999999", s));
    OK(f == -1);   // a refusal leaves the frame alone

    // Timecode from the session's origin, 01:00:00:00 at 25 fps.
    OK(parse(TC, "01:00:00:00", s) && f == 0);
    OK(parse(TC, "01:00:01:05", s) && f == kRate + 5 * kRate / 25);
    OK(!parse(TC, "00:59:59:24", s) && has(why, "before the session start"));
    OK(!parse(TC, "01:00:10", s) && has(why, "all four fields"));
    OK(!parse(TC, "01:00:00:25", s) && has(why, "25 frames a second"));
    OK(!parse(TC, "01:60:00:00", s));
    OK(!parse(TC, "24:00:00:00", s));
    // Drop-frame: a skipped label is refused, never moved to ;02.
    OK(!parse(TC, "00:01:00;00", df) && has(why, "skips"));
    OK(!parse(TC, "00:01:00;01", df));
    OK(parse(TC, "00:01:00;02", df) && format_timecode(f, df) == "00:01:00;02");
    OK(parse(TC, "00:10:00;00", df) && format_timecode(f, df) == "00:10:00;00");

    // Min:Secs, seconds last; one field is any number of seconds.
    OK(parse(MS, "83.5", s) && f == 83 * kRate + kRate / 2);
    OK(parse(MS, "1:23.5", s) && f == 83 * kRate + kRate / 2);
    OK(parse(MS, "1:02:03", s) && f == 3723 * kRate);
    OK(parse(MS, "0.001", s) && f == 48);
    OK(!parse(MS, "1:75", s) && has(why, "0 to 59"));
    OK(!parse(MS, "1:2:3:4", s));
    OK(!parse(MS, "1.2.3", s));
    OK(!parse(MS, "1:", s));

    // Samples, and the last frame transport.locate takes.
    OK(parse(SA, "48000", s) && f == 48000);
    OK(!parse(SA, "48,000", s));
    OK(parse(SA, std::to_string(kMaxLocateFrame), s) &&
       f == static_cast<Frames>(kMaxLocateFrame));
    OK(!parse(SA, std::to_string(kMaxLocateFrame + 1), s) && has(why, "past the last frame"));

    // What a counter shows, typed back, lands where it reads the same, in
    // every format, across the tempo and meter changes and in drop-frame:
    // typing the reading you see never moves you somewhere that reads
    // differently.
    int checked = 0;
    const SessionSettings* sets[] = {&s, &df};
    for (const SessionSettings* ss : sets) {
        for (TimeFormat fmt : {BB, TC, MS, SA}) {
            for (Frames p = 0; p < kAt34 + 20 * kBar96; p += 7919) {
                const std::string shown = format_time(fmt, p, *ss);
                Frames back = -1;
                std::string w;
                const bool parsed = parse_time(fmt, shown, *ss, back, w);
                OK(parsed);
                OK(format_time(fmt, back, *ss) == shown);
                if (!parsed || format_time(fmt, back, *ss) != shown) {
                    std::fprintf(stderr, "  %s at %lld: \"%s\" -> %lld (%s)\n",
                                 time_format_name(fmt), static_cast<long long>(p), shown.c_str(),
                                 static_cast<long long>(back), w.c_str());
                    return;
                }
                ++checked;
            }
        }
    }
    std::printf("test_ui_transport: %d counter readings typed back\n", checked);
}

// The Tempo and Meter rulers: a mark and a value at each change of that
// value, the value in force pinned at the left edge between changes.
void test_conductor_ruler() {
    const SessionSettings s = mapped();
    ViewState v;
    v.width = 1400;
    v.fpp = kBar120 / 50.0;   // 50 px a bar at 120
    Scene tempo, meter;
    build_conductor_ruler_scene(ConductorRuler::Tempo, s, v, 20, tempo);
    build_conductor_ruler_scene(ConductorRuler::Meter, s, v, 20, meter);
    // 96 4/4 then 96 3/4: the second is a change of meter, not of tempo.
    OK(tempo.labels.size() == 2);
    OK(meter.labels.size() == 2);
    if (tempo.labels.size() == 2 && meter.labels.size() == 2) {
        OK(tempo.labels[0].text == "120.00" && tempo.labels[1].text == "96.00");
        OK(meter.labels[0].text == "4/4" && meter.labels[1].text == "3/4");
        const double x96 = v.x_of(kAt96), x34 = v.x_of(kAt34);
        OK(std::abs(tempo.labels[1].x - (x96 + 5)) < 1.5);
        OK(std::abs(meter.labels[1].x - (x34 + 5)) < 1.5);
        // The first value runs up to the change, never under its label.
        OK(tempo.labels[0].x + tempo.labels[0].max_w <= x96);
        const SceneRect* mark = rect_at(tempo.rects, static_cast<float>(x96), 10);
        OK(mark != nullptr && mark->argb == theme::kConductor);
    }
    // Scrolled past the change: 96 is pinned at the left edge, dimmer.
    v.scroll = static_cast<double>(kAt96 + kBar96);
    build_conductor_ruler_scene(ConductorRuler::Tempo, s, v, 20, tempo);
    OK(tempo.labels.size() == 1);
    if (tempo.labels.size() == 1) {
        OK(tempo.labels[0].text == "96.00" && tempo.labels[0].x < 10);
        OK(tempo.labels[0].argb == theme::kTextSecondary);
    }
    // An entry out of order is not a change, as every reader of the map has it.
    SessionSettings odd = s;
    odd.tempo.push_back(TempoEntry{kAt96 / 2, 200.0f, {{4, 4}}});
    v.scroll = 0;
    build_conductor_ruler_scene(ConductorRuler::Tempo, odd, v, 20, tempo);
    OK(tempo.labels.size() == 2);
}

// --- loop, loop record and punch from the bar ----------------------------------

// The bar and the Transport window over one document, in the real QML, as
// main.cpp wires them.
struct BarRig {
    test::RecordingAdapter engine{18, api::testing::FakeEngine::engine_commands()};
    KeyMap map;
    ModelDocument document;
    SyntheticPeakSource peaks{kRate, 120.0};
    SessionModel session{document, peaks, true};
    KeyRouter router;
    TransportModel transport{
        [this]() -> const SessionSettings& { return session.controller().snapshot().settings; },
        [this]() { return session.controller().selection().insertion; }};
    ShellModel model;
    QQuickView view;
    QQuickItem* root = nullptr;

    static KeyMap load(const char* file) {
        KeyMap m;
        std::string err;
        OK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/" + file, m, err));
        return m;
    }
    BarRig(const char* profile, model::Snapshot demo)
        : map(load(profile)),
          document(std::move(demo)),
          router(map, engine.info().implemented_commands,
                 [this](const std::string& c, const CommandContext& ctx) {
                     return engine.send(c, ctx);
                 }),
          model(engine, router) {
        session.installCommands(router.local());
        transport.installCommands(router.local());
        router.setPlayRange([this]() {
            const auto& sel = session.controller().selection();
            return play_range_argument(sel.start, sel.end);
        });
        router.prepare(qt_device_chords(true));
        link_transport(model, session, transport);
        view.resize(1280, 720);
        view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                                   {QStringLiteral("session"), QVariant::fromValue(&session)}});
        view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
        for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
        OK(view.status() == QQuickView::Ready);
        if (view.status() != QQuickView::Ready) return;
        root = view.rootObject();
        root->setProperty("transport", QVariant::fromValue(&transport));
        view.show();
        model.tick();
    }
    QQuickItem* item(const char* name) const {
        return root ? named(root, QString::fromLatin1(name)) : nullptr;
    }
    bool active(const char* name) const {
        QQuickItem* i = item(name);
        return i != nullptr && i->isVisible() && i->property("active").toBool();
    }
    const model::Transport& tr() { return session.controller().snapshot().settings.transport; }
    std::vector<std::string> sent() const {
        std::vector<std::string> out;
        for (const auto& s : engine.sent) out.push_back(s.command);
        return out;
    }
};

// BACKLOG ui.transport (ADR-0016, update "loop record as a record mode"):
// the bar's LOOP indicator read loop_playback alone, so Loop Record on with
// Loop Playback off armed a loop take with nothing on the bar to say so.
// The bar now has LOOP and LOOP REC apart, in every profile, each sending
// its key's command.
void test_bar_loop_record() {
    for (const char* profile : {"protools.json", "logic.json"}) {
        model::Snapshot demo = make_demo_session(4, kRate);
        demo.settings.transport.loop = {{kRate, 3 * kRate}};
        demo.settings.transport.loop_record = true;
        demo.settings.transport.loop_playback = false;
        BarRig r(profile, std::move(demo));
        if (r.root == nullptr) return;
        const bool cycle = std::string(profile) == "logic.json";
        QQuickItem* loop = r.item("barLoop");
        QQuickItem* rec = r.item("barLoopRecord");
        OK(loop != nullptr && rec != nullptr);
        if (loop == nullptr || rec == nullptr) return;
        OK(rec->isVisible());
        OK(r.active("barLoopRecord"));
        OK(!r.active("barLoop"));
        OK(rec->property("text").toString() == QStringLiteral("LOOP REC"));
        OK(loop->property("text").toString() ==
           (cycle ? QStringLiteral("CYCLE") : QStringLiteral("LOOP")));
        // Each chip sends what its key sends.
        click(r.view, loop);
        click(r.view, rec);
        const auto s = r.sent();
        OK(s.size() == 2);
        if (s.size() == 2) {
            OK(s[0] == (cycle ? "transport.toggle_cycle" : "transport.toggle_loop"));
            OK(s[1] == "transport.toggle_loop_record");
        }
    }
}

// The punch range from the bar's PUNCH chip and the Transport window, and
// the rolls typed into the window's fields. #156 review N1 and N2: these
// were edits of this window's copy of the settings, sent whole, so a copy
// behind the engine's could revert a field the engine had changed, and a
// punch edit through a stale recording overlay could move the range a take
// used. Each is now an engine command carrying only what changed: the
// selection, or the roll in frames. Nothing is edited here; the engine's
// answer (its refusals with no selection, with loop record on and during a
// take are conformance's) comes back through the replica.
void test_punch_from_the_ui() {
    model::Snapshot demo = make_demo_session(4, kRate);
    demo.settings.transport.punch = {{0, 0}};
    BarRig r("protools.json", std::move(demo));
    if (r.root == nullptr) return;
    QQuickItem* punch = r.item("barPunch");
    OK(punch != nullptr);
    if (punch == nullptr) return;
    OK(!r.active("barPunch"));
    const auto last = [&r]() {
        return r.engine.sent.empty() ? test::RecordingAdapter::Sent{}
                                     : r.engine.sent.back();
    };

    // No selection: sent with none, for the engine to refuse.
    r.session.controller().select_range(0, 2 * kRate, 2 * kRate);
    click(r.view, punch);
    OK(last().command == "transport.punch_selection" && last().argument.empty());

    // A selection goes as the argument; this window's settings stay as the
    // engine last sent them.
    r.session.controller().select_range(0, 2 * kRate, 5 * kRate);
    click(r.view, punch);
    OK(last().command == "transport.punch_selection" && last().argument == "96000-240000");
    OK(r.tr().punch[1] <= r.tr().punch[0]);

    // Once the engine's range arrives the chip lights, the window shows it,
    // and the chip sends clear_punch.
    SessionSettings st = r.session.controller().snapshot().settings;
    st.transport.punch = {{2 * kRate, 5 * kRate}};
    std::string why;
    OK(r.document.submit(model::SetSettings{st}, why));
    r.session.documentUpdated();   // as the replica's update calls it
    r.model.tick();
    OK(r.active("barPunch"));
    OK(r.session.punchOn());
    OK(r.transport.punchSet());
    QQuickItem* shown = r.item("transportPunch");
    OK(shown != nullptr && shown->property("text").toString() != QStringLiteral("not set"));
    click(r.view, punch);
    OK(last().command == "transport.clear_punch");
    OK(r.tr().punch[0] == 2 * kRate && r.tr().punch[1] == 5 * kRate);

    // The Transport window's Set and Clear send the same commands.
    r.transport.setOpen(true);
    QQuickItem* set = r.item("transportPunchSet");
    QQuickItem* clear = r.item("transportPunchClear");
    OK(set != nullptr && clear != nullptr && set->isVisible());
    if (set && clear) {
        click(r.view, set);
        OK(last().command == "transport.punch_selection" && last().argument == "96000-240000");
        click(r.view, clear);
        OK(last().command == "transport.clear_punch");
    }

    // Pre- and post-roll, typed in seconds, sent in frames.
    const Frames pre_before = r.tr().pre_roll;
    OK(r.session.setPreRollText(QStringLiteral("1.5")));
    OK(last().command == "transport.set_pre_roll" && last().argument == "72000");
    OK(r.tr().pre_roll == pre_before);
    OK(r.session.setPostRollText(QStringLiteral("0")));
    OK(last().command == "transport.set_post_roll" && last().argument == "0");
    const std::size_t sent = r.engine.sent.size();
    OK(!r.session.setPreRollText(QStringLiteral("-2")));
    OK(r.engine.sent.size() == sent);
    // Through the field itself: click, type, Return.
    QQuickItem* field = r.item("transportPreRoll");
    QQuickItem* input = r.item("transportPreRollInput");
    OK(field != nullptr && input != nullptr);
    if (field && input) {
        click(r.view, field);
        OK(input->isVisible());
        type(r.view, "3");
        key(r.view, Qt::Key_Return, Qt::NoModifier);
        OK(last().command == "transport.set_pre_roll" && last().argument == "144000");
        OK(!input->isVisible());
    }

    // During a take the overlay may be stale either way, so the window does
    // not decide: the command goes, and the engine refuses it if a take is
    // open.
    r.engine.snapshot.state = TransportMode::Recording;
    for (int i = 0; i < 4; ++i) r.model.tick();
    OK(r.model.recording());
    r.model.invoke(QStringLiteral("transport.punch_selection"), -1);
    OK(last().command == "transport.punch_selection" && last().argument == "96000-240000");
    r.engine.snapshot.state = TransportMode::Stopped;
    for (int i = 0; i < 4; ++i) r.model.tick();
}

}  // namespace

// --- "Locating..." after 100 ms, and the playback misses -------------------------

void test_locating() {
    SessionSettings settings = mapped();
    TransportModel t([&settings]() -> const SessionSettings& { return settings; },
                     []() { return Frames{0}; });
    uint64_t now = 1'000'000'000;
    t.setClock([&now]() { return now; });

    TransportSnapshot snap;
    snap.state = TransportMode::Rolling;
    snap.locating = true;
    t.onTransport(snap, true);
    OK(!t.locating());                       // a locate is routinely this short
    OK(t.stateText() == QStringLiteral("PLAY"));
    now += 99'000'000;
    t.onTransport(snap, true);
    OK(!t.locating());
    now += 1'000'000;
    QSignalSpy states(&t, &TransportModel::stateChanged);
    t.onTransport(snap, true);
    OK(t.locating() && states.count() == 1);
    OK(t.stateText() == QStringLiteral("Locating..."));

    // A stale read is one tick's miss, not the end of the wait.
    TransportSnapshot stale;
    stale.stale = true;
    t.onTransport(stale, true);
    OK(t.locating());

    // Cleared by the engine: gone at once, and a later locate waits again.
    snap.locating = false;
    t.onTransport(snap, true);
    OK(!t.locating() && t.stateText() == QStringLiteral("PLAY"));
    snap.locating = true;
    now += 500'000'000;
    t.onTransport(snap, true);
    OK(!t.locating());

    // A dead engine is not locating, and its misses are not shown.
    now += 500'000'000;
    t.onTransport(snap, true);
    OK(t.locating());
    snap.playback_misses = 3;
    t.onTransport(snap, true);
    OK(t.playbackMisses() == 3);
    t.onTransport(snap, false);
    OK(!t.locating() && t.playbackMisses() == 0);
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    test_reading();
    test_grid();
    test_model();
    test_long_map();
    test_counter_parse();
    test_conductor_ruler();
    test_window();
    test_bar_loop_record();
    test_punch_from_the_ui();
    test_locating();
    if (failures != 0) {
        std::fprintf(stderr, "test_ui_transport: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_ui_transport: ok\n");
    return 0;
}
