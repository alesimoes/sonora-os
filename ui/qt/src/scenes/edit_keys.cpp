// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Keys menu open over the Edit window: the profiles, and the a-z button.
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-keys");
    s.args = QStringLiteral("--keymap-dir keymaps");
    s.setup = [](ShotContext& c) {
        if (QObject* panel = c.view.rootObject()->findChild<QObject*>(QStringLiteral("profilePanel"))) {
            panel->setProperty("visible", true);
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
