// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track > Process Selection With (plugins.offline; ADR-0087): a range on the
// kick, whose compressor and gain both play, and the submenu that lists them,
// opened beside the Track menu. The demo's fake engine announces
// track.render.insert, so the entry is live.
#include <QQuickItem>
#include <QVariant>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void open_process_with(ShotContext& c) {
    QQuickItem* root = c.view.rootObject();
    auto* button = root->findChild<QQuickItem*>(QStringLiteral("trackMenuButton"));
    auto* menu = root->findChild<QQuickItem*>(QStringLiteral("trackMenu"));
    auto* with = root->findChild<QQuickItem*>(QStringLiteral("trackProcessWith"));
    auto* sub = root->findChild<QQuickItem*>(QStringLiteral("processWithMenu"));
    if (button == nullptr || menu == nullptr || with == nullptr || sub == nullptr) return;
    const QPointF at = button->mapToScene(QPointF(0, button->height()));
    menu->setProperty("x", at.x());
    menu->setProperty("y", at.y() + 4);
    menu->setProperty("visible", true);
    const QPointF beside = with->mapToScene(QPointF(with->width(), 0));
    QMetaObject::invokeMethod(sub, "show", Q_ARG(QVariant, beside.x() + 8),
                              Q_ARG(QVariant, beside.y()));
}

const ShotRegistrar process_with{[] {
    ShotScene s;
    s.name = QStringLiteral("process-with");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            c.session.controller().select_range(0, 48000, 4 * 48000);
            QMetaObject::invokeMethod(&c.session, "selectionChanged");
            open_process_with(c);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
