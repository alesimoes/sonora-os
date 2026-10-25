// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window on the demo session, a take rolling.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit");
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
