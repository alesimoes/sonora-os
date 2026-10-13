// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real shell -- Main.qml, the meters, ShellModel's key filter -- loaded
// on the offscreen platform and driven with Qt key events. Nothing is
// rendered: this is about wiring, so it runs on any machine with Qt and no
// display. What it looks like is the screenshot's job (tools/build-ui.sh).
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>

#include "key_router.h"
#include "meter_item.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "shell_model.h"
#include "synthetic_engine.h"

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

void key(QQuickView& view, int k, Qt::KeyboardModifiers m, bool repeat = false) {
    QKeyEvent press(QEvent::KeyPress, k, m, QString(), repeat);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m, QString(), repeat);
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

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();

    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));

    test::RecordingAdapter engine(18, SyntheticEngine::engine_commands());
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.prepare(qt_device_chords());

    uint64_t now = 1;
    ShellModel model(engine, router);
    model.setClock([&now]() { return now; });

    QQuickView view;
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    model.routeKeysFrom(&view);

    // --- one meter per UMC1820 input, each attached to the one tick ---
    CHECK(meters(view).size() == 18);
    CHECK(model.attachedMeters() == 18);
    QList<int> channels;
    for (MeterItem* m : meters(view)) channels << m->channel();
    std::sort(channels.begin(), channels.end());
    for (int i = 0; i < 18; ++i) CHECK(channels[i] == i);

    // --- the tick pulls one peak per meter and feeds the right one ---
    engine.peaks[4] = 0.5f;   // -6 dBFS on input 5
    engine.peaks[9] = 1.0f;   // full scale on input 10
    model.tick();
    CHECK(engine.peak_reads == 18);
    for (MeterItem* m : meters(view)) {
        const auto& b = m->ballistics();
        CHECK(b.has_data);
        if (m->channel() == 4) CHECK(b.bar_db > -6.1f && b.bar_db < -5.9f);
        else if (m->channel() == 9) CHECK(b.over);
        else CHECK(b.bar_db == kSilenceDb);
    }

    // --- keys go through the profile, not through Qt's focus chain ---
    key(view, Qt::Key_NumberSign, Qt::ShiftModifier);  // shift+3: arm 3
    CHECK(engine.sent.size() == 1);
    CHECK(engine.sent[0].command == "track.toggle_arm" && engine.sent[0].target == 2);

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
    key(view, Qt::Key_At, Qt::ShiftModifier);  // shift+2
    model.tick();
    CHECK(model.statusText().contains(QStringLiteral("arming cannot change")));
    engine.next_result = DispatchResult::Ok;

    // An unimplemented command is said out loud without reaching the engine.
    const std::size_t before = engine.sent.size();
    key(view, Qt::Key_Z, Qt::ControlModifier);  // edit.undo
    CHECK(engine.sent.size() == before);
    CHECK(model.statusText().contains(QStringLiteral("not implemented")));

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

    // --- a dead engine: no data, not frozen values ---
    engine.alive = false;
    const int reads = engine.peak_reads;
    for (int i = 0; i < 4; ++i) model.tick();
    CHECK(!model.engineAlive());
    CHECK(engine.peak_reads == reads);  // nothing is read from a dead engine
    for (MeterItem* m : meters(view)) CHECK(!m->ballistics().has_data);
    CHECK(model.transportText() == QStringLiteral("NO ENGINE"));
    CHECK(!model.recording());
    CHECK(model.clockText().startsWith(QStringLiteral("--")));
    // ...and the over that happened before is still latched.
    for (MeterItem* m : meters(view)) {
        if (m->channel() == 9) CHECK(m->ballistics().over);
    }

    engine.alive = true;
    engine.peaks[0] = 0.25f;
    model.tick();
    CHECK(model.engineAlive());
    for (MeterItem* m : meters(view)) {
        if (m->channel() == 0) CHECK(m->ballistics().has_data && m->ballistics().bar_db > -12.1f);
    }

    std::printf("ui shell: all checks passed\n");
    return 0;
}
