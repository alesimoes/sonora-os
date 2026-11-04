// SPDX-License-Identifier: GPL-3.0-or-later
//
// The master's loudness in the interface (ui.mixer.loudness):
//   - a reading is formatted as the engine published it, and "none yet" (-200)
//     is a dash, never a quiet number;
//   - a dead engine is "no data", not the last reading;
//   - the target presets judge the integrated value (tolerance) and the true
//     peak (ceiling);
//   - reset sends meters.reset master, and every case where it cannot is
//     refused with a reason that is held, not flashed;
//   - the state block's loudness reaches the model through ShellModel's tick
//     and ControlAdapter, from FakeEngine;
//   - the QML: the panel opens beside the master and shows the numbers.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "key_router.h"
#include "loudness_model.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/op_codec.h"
#include "theme.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

struct Spy {
    std::vector<QString> resets;
    bool implemented = true;
    bool send_ok = true;
    LoudnessModel model{[this](const QString& a) {
                            resets.push_back(a);
                            return send_ok;
                        },
                        [this]() { return implemented; }};
};

EngineFrame frame_with(float m, float s, float i, float lra, float tp) {
    EngineFrame f;
    f.master_loudness.momentary = m;
    f.master_loudness.short_term = s;
    f.master_loudness.max_momentary = m + 3.0f;
    f.master_loudness.max_short_term = s + 1.0f;
    f.master_loudness.integrated = i;
    f.master_loudness.range_lu = lra;
    f.master_loudness.true_peak_max_db = tp;
    return f;
}

int formatting() {
    CHECK(LoudnessModel::level(-200.0f) == QStringLiteral("—"));
    CHECK(LoudnessModel::level(-23.44f) == QStringLiteral("-23.4"));

    Spy s;
    // Nothing measured yet: dashes, with the engine alive.
    s.model.onFrame(EngineFrame{}, true);
    CHECK(s.model.alive());
    CHECK(s.model.integratedText() == QStringLiteral("—"));
    CHECK(s.model.rangeText() == QStringLiteral("—"));
    CHECK(s.model.truePeakText() == QStringLiteral("—"));

    s.model.onFrame(frame_with(-21.8f, -22.6f, -23.2f, 6.44f, -2.3f), true);
    CHECK(s.model.momentaryText() == QStringLiteral("-21.8"));
    CHECK(s.model.shortTermText() == QStringLiteral("-22.6"));
    CHECK(s.model.integratedText() == QStringLiteral("-23.2"));
    CHECK(s.model.rangeText() == QStringLiteral("6.4"));
    CHECK(s.model.truePeakText() == QStringLiteral("-2.3"));
    CHECK(s.model.maxMomentaryText() == QStringLiteral("-18.8"));

    // The engine going away empties every reading, however loud the last one.
    s.model.onFrame(frame_with(-10.0f, -10.0f, -10.0f, 3.0f, 0.5f), false);
    CHECK(!s.model.alive());
    CHECK(s.model.momentaryText() == QStringLiteral("—"));
    CHECK(s.model.integratedText() == QStringLiteral("—"));
    CHECK(!s.model.canReset());
    CHECK(s.model.verdict() == 0);

    // Faulty samples are said, not hidden in a silent reading.
    EngineFrame f = frame_with(-30.0f, -30.0f, -30.0f, 1.0f, -9.0f);
    f.master_loudness.nonfinite = 12;
    s.model.onFrame(f, true);
    CHECK(s.model.warning().contains(QStringLiteral("12")));
    return 0;
}

int targets() {
    Spy s;
    CHECK(s.model.presets().size() == 4);
    CHECK(s.model.preset() == -1);
    s.model.onFrame(frame_with(-20.0f, -21.0f, -23.2f, 5.0f, -3.0f), true);
    CHECK(s.model.verdict() == 0);   // no target, no verdict
    CHECK(s.model.deltaText().isEmpty());

    s.model.setPreset(2);   // EBU R128: -23 +-0.5, -1 dBTP
    CHECK(s.model.targetText() == QStringLiteral("EBU R128 -23 LUFS"));
    CHECK(s.model.verdict() == 1);
    CHECK(s.model.deltaText() == QStringLiteral("-0.2 LU"));
    CHECK(!s.model.truePeakOver());

    s.model.onFrame(frame_with(-20.0f, -21.0f, -21.9f, 5.0f, -3.0f), true);
    CHECK(s.model.verdict() == 2);
    CHECK(s.model.deltaText() == QStringLiteral("+1.1 LU"));

    // The same reading against streaming's -14 is 7.9 LU under.
    s.model.setPreset(0);
    CHECK(s.model.verdict() == 2);
    CHECK(s.model.deltaText() == QStringLiteral("-7.9 LU"));

    // A true peak over the preset's ceiling is flagged; at it, not.
    s.model.onFrame(frame_with(-14.0f, -14.0f, -14.0f, 5.0f, -0.4f), true);
    CHECK(s.model.verdict() == 1);
    CHECK(s.model.truePeakOver());
    s.model.onFrame(frame_with(-14.0f, -14.0f, -14.0f, 5.0f, -1.0f), true);
    CHECK(!s.model.truePeakOver());
    // ATSC's ceiling is lower.
    s.model.setPreset(3);
    CHECK(s.model.truePeakOver());

    // Out-of-range indices are ignored; -1 clears.
    s.model.setPreset(9);
    CHECK(s.model.preset() == 3);
    s.model.setPreset(-1);
    CHECK(s.model.targetText() == QStringLiteral("No target"));
    return 0;
}

int resetting() {
    Spy s;
    // Dead engine: refused, with the reason, and nothing sent.
    CHECK(!s.model.reset());
    CHECK(s.resets.empty());
    CHECK(s.model.refusal().contains(QStringLiteral("not responding")));

    s.model.onFrame(frame_with(-20.0f, -21.0f, -23.0f, 5.0f, -3.0f), true);
    s.implemented = false;
    CHECK(!s.model.canReset());
    CHECK(!s.model.reset());
    CHECK(s.resets.empty());
    CHECK(s.model.refusal().contains(QStringLiteral("meters.reset")));

    s.implemented = true;
    s.send_ok = false;
    CHECK(!s.model.reset());
    CHECK(s.model.refusal().contains(QStringLiteral("not sent")));

    s.send_ok = true;
    CHECK(s.model.reset());
    CHECK(s.resets.size() == 2);
    CHECK(s.resets.back() == QStringLiteral("master"));
    CHECK(s.model.refusal().isEmpty());

    // A refusal and a target belong to the session that had them.
    s.implemented = false;
    s.model.reset();
    s.model.setPreset(1);
    CHECK(!s.model.refusal().isEmpty());
    s.model.sessionOpened();
    CHECK(s.model.refusal().isEmpty());
    CHECK(s.model.preset() == -1);
    return 0;
}

int through_the_shell() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 2;
    api::testing::FakeEngine fake{cfg};
    ControlAdapter engine{fake};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    router.prepare({});
    ShellModel shell{engine, router};
    std::string error;
    CHECK(engine.connect(error));

    std::vector<std::string> sent;
    LoudnessModel model(
        [&shell, &sent](const QString& a) {
            sent.push_back(a.toStdString());
            shell.invokeWith(QStringLiteral("meters.reset"), -1, a);
            return true;
        },
        [&shell]() { return shell.implemented(QStringLiteral("meters.reset")); });
    int calls = 0;
    shell.setLoudnessSink([&model, &calls](const EngineFrame& f, bool alive) {
        ++calls;
        model.onFrame(f, alive);
    });
    for (int i = 0; i < 12; ++i) shell.tick();
    CHECK(calls > 0);
    CHECK(model.alive());
    // The fake measures nothing: the ControlAdapter hands on "none yet".
    CHECK(model.integratedText() == QStringLiteral("—"));
    CHECK(shell.implemented(QStringLiteral("meters.reset")));
    CHECK(model.canReset());
    CHECK(model.reset());
    CHECK(sent.size() == 1 && sent[0] == "master");
    return 0;
}

int qml() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 2;
    api::testing::FakeEngine fake{cfg};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[&engine](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(2, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    router.prepare({});
    ShellModel shell{engine, router};
    std::string error;
    CHECK(engine.connect(error));
    Spy spy;

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)},
                               {QStringLiteral("loudness"), QVariant::fromValue(&spy.model)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    session.setWindow(QStringLiteral("mix"));
    view.show();

    QQuickItem* root = view.rootObject();
    auto item = [root](const char* name) { return root->findChild<QQuickItem*>(name); };
    QQuickItem* panel = item("loudnessPanel");
    CHECK(panel != nullptr);
    CHECK(!panel->isVisible());   // closed until the Loudness button opens it
    QQuickItem* button = item("viewLoudness");
    // Its row shows with the mixer, which this rig does not wire.
    CHECK(button != nullptr);

    spy.model.onFrame(frame_with(-21.8f, -22.6f, -23.2f, 6.4f, -0.5f), true);
    spy.model.setPreset(2);
    spy.model.setPanelOpen(true);
    CHECK(panel->isVisible());
    CHECK(item("loudnessIntegrated")->property("text").toString() == QStringLiteral("-23.2"));
    CHECK(item("loudnessMomentary")->property("text").toString() == QStringLiteral("-21.8"));
    CHECK(item("loudnessTruePeak")->property("text").toString() == QStringLiteral("-0.5"));
    CHECK(item("loudnessDelta")->isVisible());
    CHECK(item("loudnessCeiling")->property("text").toString().contains(QStringLiteral("exceeded")));
    CHECK(!item("loudnessNoData")->isVisible());

    // Engine gone: the panel says so and shows dashes, not -23.2.
    spy.model.onFrame(EngineFrame{}, false);
    CHECK(item("loudnessNoData")->isVisible());
    CHECK(item("loudnessIntegrated")->property("text").toString() == QStringLiteral("—"));

    // A refused reset is on the panel, held.
    spy.model.reset();
    CHECK(item("loudnessRefusal")->isVisible());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (formatting() != 0) return 1;
    if (targets() != 0) return 1;
    if (resetting() != 0) return 1;
    if (through_the_shell() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui loudness: all checks passed\n");
    return 0;
}
