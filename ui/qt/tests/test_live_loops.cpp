// SPDX-License-Identifier: GPL-3.0-or-later
//
// LiveLoopsModel over the real state machine: the grid in the state the
// launcher reports, scene and cell launch, quantise selection, a refusal
// held as a notice (and dismissed), and the QML grid loading against it.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariantMap>
#include <cstdio>

#include "live_loops_model.h"
#include "register_types.h"
#include "sonora/launch/state_machine_launcher.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

struct Rig {
    launch::StateMachineLauncher launcher{[] {
        launch::LauncherConfig c;
        c.tracks = 3;
        c.scenes = 2;
        c.quantize = launch::Quantize::Immediate;
        return c;
    }()};
    LiveLoopsModel model{launcher, [this]() -> const launch::LaunchGrid& { return launcher.grid(); }};
    Rig() {
        launcher.set_transport(launch::TransportSnapshot{});
        for (std::uint32_t t = 0; t < 3; ++t) launcher.assign_clip(t, 0, 10 + t);
        launcher.assign_clip(0, 1, 20);  // (1, 1) and (2, 1) stay empty
    }
    QString state(int track, int scene) const {
        return model.scenes().at(scene).toMap().value("cells").toList().at(track).toMap()
            .value("state").toString();
    }
    bool hasClip(int track, int scene) const {
        return model.scenes().at(scene).toMap().value("cells").toList().at(track).toMap()
            .value("hasClip").toBool();
    }
};

int grid_follows_launcher() {
    Rig r;
    CHECK(r.model.trackCount() == 3 && r.model.scenes().size() == 2);
    CHECK(r.hasClip(0, 1) && !r.hasClip(1, 1));
    CHECK(r.state(0, 0) == "stopped");
    r.model.triggerScene(0);
    r.launcher.advance(0, 256);
    CHECK(r.state(0, 0) == "playing" && r.state(2, 0) == "playing");
    r.model.stopAll();
    r.launcher.advance(256, 256);
    CHECK(r.state(0, 0) == "stopped");
    return 0;
}

int quantise_and_queued() {
    Rig r;
    CHECK(r.model.quantizeNames().size() == 5 && r.model.quantize() == 2);
    r.model.setQuantize(2);
    r.launcher.set_transport(launch::TransportSnapshot{});
    r.model.triggerCell(0, 1);
    r.launcher.advance(0, 256);
    CHECK(r.state(0, 1) == "queued");  // waits for the bar, never shown playing early
    r.model.setQuantize(99);
    CHECK(r.model.quantize() == 2);
    return 0;
}

int refusal_is_held() {
    Rig r;
    r.model.triggerCell(7, 7);  // outside the grid: the launcher refuses
    CHECK(!r.model.notice().isEmpty());
    CHECK(r.state(0, 0) == "stopped");
    r.model.dismissNotice();
    CHECK(r.model.notice().isEmpty());
    return 0;
}

int qml_loads() {
    Rig r;
    r.model.triggerScene(0);
    r.launcher.advance(0, 256);
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent comp(&engine, QUrl(QStringLiteral("qrc:/sonora/qml/LiveLoops.qml")));
    QVariantMap props;
    props.insert("loops", QVariant::fromValue(static_cast<QObject*>(&r.model)));
    QObject* o = comp.createWithInitialProperties(props);
    if (o == nullptr) std::fprintf(stderr, "%s\n", qPrintable(comp.errorString()));
    CHECK(o != nullptr);
    auto* item = qobject_cast<QQuickItem*>(o);
    CHECK(item->findChild<QQuickItem*>("llCell0_0")->property("cellState").toString() == "playing");
    CHECK(item->findChild<QQuickItem*>("llScene1") != nullptr);
    CHECK(item->findChild<QQuickItem*>("llQuantize4") != nullptr);
    delete o;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (int rc = grid_follows_launcher()) return rc;
    if (int rc = quantise_and_queued()) return rc;
    if (int rc = refusal_is_held()) return rc;
    if (int rc = qml_loads()) return rc;
    std::puts("ok");
    return 0;
}
