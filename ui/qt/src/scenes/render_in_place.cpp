// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place (plugins.graph.freeze, plugins.offline; ADR-0051): a track
// selected and Track > Freeze asked for, the Track menu showing the four
// commands and the held notice of how the render ended in the footer; then, as
// a second scene, the engine refusing one while the transport rolls, the
// reason held in the footer instead of the 2.5 s status line.
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void open_track_menu(ShotContext& c) {
    QQuickItem* root = c.view.rootObject();
    auto* button = root->findChild<QQuickItem*>(QStringLiteral("trackMenuButton"));
    auto* menu = root->findChild<QQuickItem*>(QStringLiteral("trackMenu"));
    if (button == nullptr || menu == nullptr) return;
    const QPointF at = button->mapToScene(QPointF(0, button->height()));
    menu->setProperty("x", at.x());
    menu->setProperty("y", at.y() + 4);
    menu->setProperty("visible", true);
}

const ShotRegistrar done{[] {
    ShotScene s;
    s.name = QStringLiteral("render-in-place");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("track.select"), c.track);
        // The demo's fake engine says Running, then Done a poll later.
        c.session.renderInPlace(QStringLiteral("freeze"));
        open_track_menu(c);
    };
    return s;
}()};

const ShotRegistrar refused{[] {
    ShotScene s;
    s.name = QStringLiteral("render-in-place-refused");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("track.select"), c.track);
        // A render needs the transport stopped.
        c.model.invoke(QStringLiteral("transport.play"), -1);
        c.session.renderInPlace(QStringLiteral("freeze"));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
