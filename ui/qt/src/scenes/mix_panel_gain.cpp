// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the snare's gain plugin open in the generic editor: a
// knob over a range through zero, lit from zero down to -9.6 dB, and a
// toggle switched on.
#include <QTimer>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-gain");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) {
        c.inserts.openPanel(1, 0);
        // The values need the plugin's catalog entry, which the engine
        // sends after the snapshot; the picture is taken seconds later.
        InsertModel* inserts = &c.inserts;
        QTimer::singleShot(1500, inserts, [inserts] {
            inserts->paramBegin();
            inserts->paramPosition(QStringLiteral("gain"), 0.3);
            inserts->paramEnd();
            inserts->paramStep(QStringLiteral("invert"), 1);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
