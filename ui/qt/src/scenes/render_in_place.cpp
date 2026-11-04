// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place (plugins.graph.freeze, plugins.offline; ADR-0051): a track
// selected, Track > Freeze asked for, the render under way in the footer with
// its Cancel while the Track menu shows the commands waiting; then, as a
// second scene, how it ended, held in the footer until dismissed.
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

void freeze(ShotContext& c) {
    c.model.invoke(QStringLiteral("track.select"), c.track);
    c.session.renderInPlace(QStringLiteral("freeze"));
    // The demo's fake engine says half, then Done a poll later.
    c.model.tick();
}

const ShotRegistrar running{[] {
    ShotScene s;
    s.name = QStringLiteral("render-in-place");
    s.take = false;
    s.apply = [](ShotContext& c) {
        freeze(c);
        open_track_menu(c);
    };
    return s;
}()};

const ShotRegistrar done{[] {
    ShotScene s;
    s.name = QStringLiteral("render-in-place-done");
    s.take = false;
    s.apply = [](ShotContext& c) {
        freeze(c);
        c.model.tick();
        open_track_menu(c);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
