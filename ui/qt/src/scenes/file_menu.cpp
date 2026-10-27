// SPDX-License-Identifier: GPL-3.0-or-later
//
// The File menu open under its button: Save, Save As, Save Copy In, Open
// (daw.model.session_mgmt).
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-file-menu");
    s.take = false;
    s.apply = [](ShotContext& c) {
        QQuickItem* root = c.view.rootObject();
        auto* button = root->findChild<QQuickItem*>(QStringLiteral("fileMenuButton"));
        auto* menu = root->findChild<QQuickItem*>(QStringLiteral("fileMenu"));
        if (button == nullptr || menu == nullptr) return;
        const QPointF at = button->mapToScene(QPointF(0, button->height()));
        menu->setProperty("x", at.x());
        menu->setProperty("y", at.y() + 4);
        menu->setProperty("visible", true);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
