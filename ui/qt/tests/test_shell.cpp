// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real shell -- Main.qml with the Edit and Mix windows, the meters,
// ShellModel's key filter, SessionModel's commands -- loaded on the
// offscreen platform and driven with Qt key events. Nothing is rendered:
// this is about wiring, so it runs on any machine with Qt and no display.
// What it looks like is the screenshot's job (tools/build-ui.sh).
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>

#include "key_router.h"
#include "meter_item.h"
#include "qt_keys.h"
#include "peak_source.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "scene_item.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
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

// As the xcb and Wayland plugins deliver a key: with the physical key's XKB
// keycode, which is how the digit row is told apart on every layout.
void key(QQuickView& view, int k, Qt::KeyboardModifiers m, bool repeat = false,
         quint32 xkb = 0) {
    QKeyEvent press(QEvent::KeyPress, k, m, xkb, 0, 0, QString(), repeat);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m, xkb, 0, 0, QString(), repeat);
    QCoreApplication::sendEvent(&view, &release);
}

// Repeater delegates are visual children, not QObject children, so
// findChildren() does not see them; walk the item tree instead.
void collect(QQuickItem* item, QList<MeterItem*>& out) {
    if (auto* m = qobject_cast<MeterItem*>(item)) out << m;
    for (QQuickItem* c : item->childItems()) collect(c, out);
}

QList<MeterItem*> meters(QQuickView& view) {
    QList<MeterItem*> out;
    collect(view.rootObject(), out);
    return out;
}

// The meters on screen that show an input (the bus's have no channel).
QList<MeterItem*> visible_meters(QQuickView& view) {
    QList<MeterItem*> out;
    for (MeterItem* m : meters(view)) {
        if (m->isVisible() && m->channel() >= 0) out << m;
    }
    return out;
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();

    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));

    test::RecordingAdapter engine(18, api::testing::FakeEngine::engine_commands());
    LocalDocument document(make_demo_session(18, 48000));
    SyntheticPeakSource peaks(48000, 120.0);
    SessionModel session(document, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));

    uint64_t now = 1;
    ShellModel model(engine, router);
    model.setClock([&now]() { return now; });
    model.setTransportSink([&session](const TransportSnapshot& t, bool alive) {
        session.onTransport(t, alive);
    });

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    model.routeKeysFrom(&view);
    // Offscreen has no keyboard; the events below carry XKB keycodes.
    CHECK(!model.scanCodesAreXkb());
    model.setScanCodesAreXkb(true);

    // --- one meter per UMC1820 input in each window, each attached to the
    // one tick; the monitor bus's two have no source and stay "no data" ---
    CHECK(meters(view).size() == 2 * 18 + 2);
    CHECK(model.attachedMeters() == 2 * 18);
    // The Edit window shows the meters of the lanes on screen, and the tick
    // feeds only those; the Mix window shows all eighteen.
    const int on_screen = static_cast<int>(visible_meters(view).size());
    CHECK(on_screen > 4 && on_screen < 18);
    session.setWindow(QStringLiteral("mix"));
    CHECK(visible_meters(view).size() == 18);
    QList<int> channels;
    for (MeterItem* m : visible_meters(view)) channels << m->channel();
    std::sort(channels.begin(), channels.end());
    for (int i = 0; i < 18; ++i) CHECK(channels[i] == i);

    // --- the tick pulls one peak per channel and feeds the right meter ---
    engine.peaks[4] = 0.5f;   // -6 dBFS on input 5
    engine.peaks[9] = 1.0f;   // full scale on input 10
    model.tick();
    CHECK(engine.peak_reads == 18);
    for (MeterItem* m : visible_meters(view)) {
        const auto& b = m->ballistics();
        CHECK(b.has_data);
        if (m->channel() == 4) CHECK(b.bar_db > -6.1f && b.bar_db < -5.9f);
        else if (m->channel() == 9) CHECK(b.over);
        else CHECK(b.bar_db == kSilenceDb);
    }

    // --- keys go through the profile, not through Qt's focus chain ---
    key(view, Qt::Key_NumberSign, Qt::ShiftModifier, false, 12);  // shift+3: arm 3
    CHECK(engine.sent.size() == 1);
    CHECK(engine.sent[0].command == "track.toggle_arm" && engine.sent[0].target == 2);
    // AZERTY's "*" key, next to Return: read the US way it was shift+8, and
    // armed track 8. A key the decoder cannot place is said to mean nothing.
    key(view, Qt::Key_Asterisk, Qt::NoModifier, false, 51);
    CHECK(engine.sent.size() == 1);
    CHECK(model.statusText() == QStringLiteral("that key means nothing here"));

    session.setWindow(QStringLiteral("edit"));

    // Auto-repeat does nothing: holding Space must not start and stop takes.
    key(view, Qt::Key_Space, Qt::ControlModifier, /*repeat=*/true);
    CHECK(engine.sent.size() == 1);
    key(view, Qt::Key_Space, Qt::ControlModifier);
    CHECK(engine.sent.size() == 2 && engine.sent[1].command == "transport.record");

    // A modifier on its own is not a key press.
    key(view, Qt::Key_Shift, Qt::ShiftModifier);
    CHECK(engine.sent.size() == 2);

    // Tab is a profile key (or unbound), never focus traversal.
    key(view, Qt::Key_Tab, Qt::NoModifier);
    CHECK(engine.sent.size() == 2);

    // --- a refusal comes back as the engine's reason, on the status line ---
    engine.next_result = DispatchResult::Failed;
    engine.next_message = "arming cannot change during a take; stop first";
    key(view, Qt::Key_At, Qt::ShiftModifier, false, 11);  // shift+2
    model.tick();
    CHECK(model.statusText().contains(QStringLiteral("arming cannot change")));
    engine.next_result = DispatchResult::Ok;

    // An unimplemented command is said out loud without reaching the engine.
    const std::size_t before = engine.sent.size();
    key(view, Qt::Key_Return, Qt::NoModifier);  // transport.return_to_zero
    CHECK(engine.sent.size() == before);
    CHECK(model.statusText().contains(QStringLiteral("not implemented")));
    // Undo is the interface's own now, and says why it did nothing.
    key(view, Qt::Key_Z, Qt::ControlModifier);
    CHECK(engine.sent.size() == before);
    CHECK(model.statusText().contains(QStringLiteral("nothing to undo")));

    // --- the Edit window: the timeline and rulers are attached and drawn
    // from the session; a key edits through it ---
    auto* timeline = qobject_cast<TimelineItem*>(named(view.rootObject(), QStringLiteral("timeline")));
    CHECK(timeline != nullptr);
    CHECK(timeline->width() > 0 && timeline->height() > 0);
    CHECK(session.controller().view().width == timeline->width());
    for (const char* r : {"ruler0", "ruler1", "ruler2"}) {
        CHECK(qobject_cast<RulerItem*>(named(view.rootObject(), QString::fromLatin1(r))) != nullptr);
    }
    key(view, Qt::Key_F4, Qt::NoModifier);  // Grid
    CHECK(session.editMode() == QStringLiteral("Grid"));
    key(view, Qt::Key_F2, Qt::NoModifier);  // Slip
    // Lead Vox (track 15) has a region at bars 5-8; put the insertion in it
    // and separate with Ctrl+E.
    session.selectTrack(14);
    session.controller().set_insertion(5 * 96000);
    const auto regions = [&document]() {
        return document.snapshot().edit->tracks[14]->active()->regions.size();
    };
    const std::size_t had = regions();
    key(view, Qt::Key_E, Qt::ControlModifier, false, 18 + 8);
    CHECK(regions() == had + 1);
    key(view, Qt::Key_Z, Qt::ControlModifier, false, 44 + 8);
    CHECK(regions() == had);
    // A drag on the timeline is a move, through the same model.
    const double fpp = session.controller().view().fpp;
    const double x = session.controller().view().x_of(5 * 96000);
    const double y = session.controller().view().y_of_track(14) + 50;
    session.press(x, y);
    session.drag(x + 40, y);
    session.release(x + 40, y);
    CHECK(document.snapshot().find_region(
              document.snapshot().edit->tracks[14]->active()->regions[0].id)->position ==
          4 * 96000 + static_cast<int64_t>(40 * fpp));
    key(view, Qt::Key_Z, Qt::ControlModifier, false, 44 + 8);

    // --- Ctrl+= switches to the Mix window and back ---
    QQuickItem* mixWindow = named(view.rootObject(), QStringLiteral("mixWindow"));
    QQuickItem* editWindow = named(view.rootObject(), QStringLiteral("editWindow"));
    CHECK(mixWindow && editWindow && !mixWindow->isVisible() && editWindow->isVisible());
    key(view, Qt::Key_Equal, Qt::ControlModifier, false, 13 + 8);
    CHECK(session.window() == QStringLiteral("mix"));
    CHECK(mixWindow->isVisible() && !editWindow->isVisible());
    CHECK(visible_meters(view).size() == 18);
    // A fader ride is mix state: kept, and not an undo step.
    const bool could_undo = document.can_undo();
    session.faderDrag(3, -20, 200, false);
    CHECK(session.gains().at(3) > -2.0 - (3 * 7) % 9);
    CHECK(document.can_undo() == could_undo);
    session.resetGain(3);
    CHECK(session.gains().at(3) == 0.0);
    key(view, Qt::Key_Equal, Qt::ControlModifier, false, 13 + 8);
    CHECK(session.window() == QStringLiteral("edit"));

    // --- Shift+M and Shift+S act on the selected track ---
    key(view, Qt::Key_M, Qt::ShiftModifier, false, 50 + 8);
    CHECK(session.mutes().at(14));
    key(view, Qt::Key_S, Qt::ShiftModifier, false, 31 + 8);
    CHECK(session.solos().at(14));

    // --- the arm button is the arm key ---
    model.invoke(QStringLiteral("track.toggle_arm"), 7);
    CHECK(engine.sent.back().command == "track.toggle_arm" && engine.sent.back().target == 7);

    // --- state is pulled, and only re-published when it changes ---
    engine.armed.assign(18, false);
    engine.armed[2] = true;
    engine.snapshot.state = TransportMode::Recording;
    engine.snapshot.playhead_frames = 48000 * 61 + 24000;  // 1:01.5
    engine.snapshot.take = 4;
    for (int i = 0; i < 4; ++i) model.tick();
    CHECK(model.armed().at(2));
    CHECK(!model.armed().at(3));
    CHECK(model.recording());
    CHECK(model.transportText() == QStringLiteral("REC"));
    CHECK(model.clockText() == QStringLiteral("00:01:01.500"));
    CHECK(model.take() == 4);
    // The recording playhead reaches the timeline as a C++ call every tick.
    CHECK(session.transportOverlay().rolling && session.transportOverlay().recording);
    CHECK(session.transportOverlay().playhead == 48000 * 61 + 24000);

    CHECK(engine.frames > 0);   // one frame per refresh, not a read per track

    // --- what the engine says is wrong is on screen, in red ---
    {
        EngineEvent failed;
        failed.kind = EngineEventKind::TakeFailed;
        failed.take = 4;
        failed.text = "take 4 failed: Audio 3_04.wav: no space left on device";
        engine.events.push_back(failed);
        EngineEvent fatal;
        fatal.kind = EngineEventKind::Fatal;
        fatal.text = "the audio server went away";
        engine.events.push_back(fatal);
        engine.snapshot.health_flags = kHealthTakeFailed;
    }
    for (int i = 0; i < 4; ++i) model.tick();
    CHECK(engine.events.empty());
    CHECK(model.alertText().startsWith(QStringLiteral("the audio server went away")));
    CHECK(model.alertText().contains(QStringLiteral("no space left on device")));
    CHECK(model.healthText().contains(QStringLiteral("take failed")));
    QQuickItem* banner = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("banner"));
    QQuickItem* banner_text =
        view.rootObject()->findChild<QQuickItem*>(QStringLiteral("bannerText"));
    CHECK(banner != nullptr && banner->isVisible());
    CHECK(banner_text != nullptr &&
          banner_text->property("text").toString() == model.alertText());
    // A take that records cleanly clears its part; the fatal reason stays.
    {
        EngineEvent cleared;
        cleared.kind = EngineEventKind::TakeFailed;
        cleared.take = 5;
        engine.events.push_back(cleared);
        engine.snapshot.health_flags = 0;
    }
    for (int i = 0; i < 4; ++i) model.tick();
    CHECK(model.alertText() == QStringLiteral("the audio server went away"));
    CHECK(!model.healthText().contains(QStringLiteral("take failed")));

    // --- a dead engine: no data, not frozen values ---
    engine.alive = false;
    const int reads = engine.peak_reads;
    for (int i = 0; i < 4; ++i) model.tick();
    CHECK(!model.engineAlive());
    CHECK(engine.peak_reads == reads);  // nothing is read from a dead engine
    for (MeterItem* m : meters(view)) CHECK(!m->ballistics().has_data);
    // ...and the playhead is not drawn from a dead engine's last value.
    CHECK(!session.transportOverlay().rolling);
    CHECK(model.transportText() == QStringLiteral("NO ENGINE"));
    CHECK(!model.recording());
    CHECK(model.clockText().startsWith(QStringLiteral("--")));
    // ...and the over that happened before is still latched.
    for (MeterItem* m : visible_meters(view)) {
        if (m->channel() == 9) CHECK(m->ballistics().over);
    }

    engine.alive = true;
    engine.peaks[0] = 0.25f;
    model.tick();
    CHECK(model.engineAlive());
    for (MeterItem* m : visible_meters(view)) {
        if (m->channel() == 0) CHECK(m->ballistics().has_data && m->ballistics().bar_db > -12.1f);
    }

    std::printf("ui shell: all checks passed\n");
    return 0;
}
