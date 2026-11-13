// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kick's compressor panel with the first knob focused from the keyboard:
// the focus ring a Tab stop draws.
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("knob-focus");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) { c.inserts.openPanel(0, 0); };
    s.apply = [](ShotContext& c) {
        QQuickItem* root = c.view.rootObject();
        if (root == nullptr) return;
        QQuickItem* knob = root->findChild<QQuickItem*>(QStringLiteral("knob_0"));
        if (knob == nullptr) {
            std::fprintf(stderr, "sonora-ui: scene: knob-focus: no knob_0 in the panel\n");
            return;
        }
        knob->forceActiveFocus(Qt::TabFocusReason);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
