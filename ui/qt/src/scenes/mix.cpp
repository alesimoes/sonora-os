// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window on the demo session, a take rolling.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix");
    s.window = QStringLiteral("mix");
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
