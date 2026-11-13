// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Live Loops grid over the edit window: a launcher on its own state
// machine, scene 1 playing, two cells queued, one track stopping and one
// cell armed to record, so every state shows.
#include <QQmlComponent>
#include <QQuickItem>
#include <QVariantMap>
#include <memory>

#include "live_loops_model.h"
#include "screenshot_scene.h"
#include "sonora/launch/state_machine_launcher.h"

namespace sonora::ui {
namespace {

struct Host {
    launch::StateMachineLauncher launcher;
    std::unique_ptr<LiveLoopsModel> model;
    explicit Host(const launch::LauncherConfig& c) : launcher(c) {}
};

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("live-loops");
    s.take = false;
    s.apply = [](ShotContext& c) {
        launch::LauncherConfig cfg;
        cfg.tracks = 6;
        cfg.scenes = 5;
        cfg.quantize = launch::Quantize::Immediate;
        auto* host = new Host(cfg);
        auto& l = host->launcher;
        l.set_transport(launch::TransportSnapshot{});
        for (std::uint32_t t = 0; t < 6; ++t)
            for (std::uint32_t sc = 0; sc < 5; ++sc)
                if ((t + sc) % 7 != 6) l.assign_clip(t, sc, 100 + t * 10 + sc);
        l.trigger_scene(0);
        l.advance(0, 256);
        l.set_quantize(launch::Quantize::Bar);
        l.trigger_cell(1, 1);
        l.trigger_cell(3, 2);
        l.stop_track(4);
        l.arm_record(5, 3, 900, 4);
        l.advance(256, 256);
        host->model = std::make_unique<LiveLoopsModel>(
            l, [host]() -> const launch::LaunchGrid& { return host->launcher.grid(); }, &c.view);
        QQmlComponent comp(c.view.engine(), QUrl(QStringLiteral("qrc:/sonora/qml/LiveLoops.qml")));
        QVariantMap props;
        props.insert(QStringLiteral("loops"), QVariant::fromValue(static_cast<QObject*>(host->model.get())));
        QObject* o = comp.createWithInitialProperties(props);
        auto* panel = qobject_cast<QQuickItem*>(o);
        if (panel == nullptr) {
            std::fprintf(stderr, "sonora-ui: live loops scene: %s\n", qPrintable(comp.errorString()));
            delete o;
            return;
        }
        panel->setParent(c.view.rootObject());
        panel->setParentItem(c.view.rootObject());
        panel->setX((c.view.rootObject()->width() - panel->width()) / 2);
        panel->setY((c.view.rootObject()->height() - panel->height()) / 2);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
