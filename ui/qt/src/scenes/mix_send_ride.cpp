// SPDX-License-Identifier: GPL-3.0-or-later
//
// A send's fader held down in its send window (ADR-0022): the cap and the
// readout follow the hand, heard live as ParamId::SendLevel, and the session
// is not touched until the release.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-send-ride");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.sends.openWindow(3, 1);
        c.sends.beginLevelRide();
        c.sends.levelDrag(-40.0, 200.0, false);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
