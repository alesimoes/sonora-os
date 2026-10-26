// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Latency (ADR-0019, update of 2026-10-26), in the real QML over
// FakeEngine's wire: the Setup menu opens the window through window.latency;
// the round trip the engine stands on is shown with its source; Measure on
// the fake's cable shows the probe at work, then its figure, measured; a
// refusal (a second measurement, a change during a take) is held in the
// window until the next action, not only on the status line; Set makes the
// typed figure L, by hand; a measurement on an uncabled input is a failure
// shown as one, which leaves L as it was.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>
#include <string>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
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

QString text_of(QQuickItem* root, const char* name) {
    QQuickItem* item = named(root, QString::fromLatin1(name));
    return item != nullptr ? item->property("text").toString() : QStringLiteral("<none>");
}

int test_window() {
    uint64_t now = 1'000'000'000;
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    cfg.clock = [&now]() { return now; };
    cfg.latency_cable = {2, 4, 287};
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));

    KeyMap map;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    ModelDocument doc(make_empty_session(4, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    model.tick();
    CHECK(model.latencyKnown() && model.latencyFrames() == 0 && model.latencyOutputs() == 2);
    CHECK(model.latencySource().startsWith(QStringLiteral("not measured")));

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();

    // Setup > Latency… opens it; the output and input start on out_2 and
    // the last input, where the loopback is usually patched.
    QQuickItem* win = named(root, QStringLiteral("latencyWindow"));
    CHECK(win != nullptr && !win->isVisible());
    click(view, named(root, QStringLiteral("setupMenuButton")));
    QQuickItem* item = named(root, QStringLiteral("setupLatency"));
    CHECK(QTest::qWaitFor([&]() { return item->isVisible(); }));
    click(view, item);
    CHECK(session.latencyOpen() && win->isVisible());
    CHECK(text_of(win, "latencyOutputValue") == QStringLiteral("out_2"));
    CHECK(text_of(win, "latencyInputValue") == QStringLiteral("in_4"));
    CHECK(text_of(win, "latencyFrames") == QStringLiteral("0 frames"));

    // Measure: the probe at work, then the cable's figure, measured.
    click(view, named(win, QStringLiteral("latencyMeasure")));
    model.tick();
    CHECK(model.latencyMeasuring());
    CHECK(text_of(win, "latencyResult").contains(QStringLiteral("measuring out_2 to in_4")));
    // A second one is refused, and the refusal stays in the window.
    click(view, named(win, QStringLiteral("latencyMeasure")));
    model.tick();
    CHECK(model.latencyRefusal().contains(QStringLiteral("already running")));
    QQuickItem* refusal = named(win, QStringLiteral("latencyRefusal"));
    CHECK(refusal != nullptr && refusal->isVisible());
    now += 2'000'000'000;
    model.tick();
    CHECK(!model.latencyMeasuring() && model.latencyFrames() == 287);
    CHECK(model.latencySource().startsWith(QStringLiteral("measured")));
    CHECK(text_of(win, "latencyFrames") == QStringLiteral("287 frames"));
    CHECK(text_of(win, "latencyResult").contains(QStringLiteral("287 frames")));
    // Held past the status line's life, until the next action.
    CHECK(refusal->isVisible());

    // By hand: the refusal goes with the next action, and L is the figure.
    QQuickItem* input = named(win, QStringLiteral("latencyManualInput"));
    CHECK(input != nullptr);
    input->setProperty("text", QStringLiteral("300"));
    click(view, named(win, QStringLiteral("latencySet")));
    CHECK(model.latencyRefusal().isEmpty() && !refusal->isVisible());
    model.tick();
    CHECK(model.latencyFrames() == 300 && model.latencySource() == QStringLiteral("set by hand"));

    // An input with no cable: a failure shown as one, L left as it was.
    for (int i = 0; i < 3; ++i) click(view, named(win, QStringLiteral("latencyInputDown")));
    CHECK(text_of(win, "latencyInputValue") == QStringLiteral("in_1"));
    click(view, named(win, QStringLiteral("latencyMeasure")));
    model.tick();
    now += 2'000'000'000;
    model.tick();
    CHECK(model.latencyFailed() && model.latencyFrames() == 300);
    CHECK(text_of(win, "latencyResult").contains(QStringLiteral("no arrival")));

    // During a take L cannot change: refused, and said in the window.
    model.invoke(QStringLiteral("track.toggle_arm"), 0);
    model.invoke(QStringLiteral("transport.record"), -1);
    model.tick();
    model.setLatency(10);
    model.tick();
    CHECK(model.latencyRefusal().contains(QStringLiteral("during a take")));
    CHECK(refusal->isVisible());
    CHECK(model.latencyFrames() == 300);

    // The window's × closes it, as window.latency does.
    click(view, named(win, QStringLiteral("latencyClose")));
    CHECK(!session.latencyOpen() && !win->isVisible());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_window()) return 1;
    std::printf("ui latency: ok\n");
    return 0;
}
