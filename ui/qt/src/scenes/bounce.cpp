// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > Bounce Mix (ui.mixer.bounce): the window opened through
// window.bounce with real time, 32-bit float and normalise to -14 LUFS chosen
// and a name typed, over the demo's fake engine; and the same window after a
// bounce ended, with its held notice in the footer.
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void choose(ShotContext& c) {
    c.model.invoke(QStringLiteral("window.bounce"), -1);
    QQuickItem* root = c.view.rootObject();
    if (auto* win = root->findChild<QQuickItem*>(QStringLiteral("bounceWindow"))) {
        win->setProperty("realTime", true);
        win->setProperty("float32", true);
        win->setProperty("normalise", true);
        win->setProperty("targetLufs", -14);
    }
    if (auto* name = root->findChild<QQuickItem*>(QStringLiteral("bounceName"))) {
        name->setProperty("text", QStringLiteral("Final Mix"));
    }
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("bounce");
    s.take = false;
    s.apply = [](ShotContext& c) { choose(c); };
    return s;
}()};

const ShotRegistrar done_registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("bounce-done");
    s.take = false;
    s.apply = [](ShotContext& c) {
        choose(c);
        // The demo's fake engine says Running, then Done a poll later.
        c.model.startBounce(true, true, true, -14.0, QStringLiteral("Final Mix"));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
