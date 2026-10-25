// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window after the engine stops answering.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-no-engine");
    s.args = QStringLiteral("--engine-dies-after 2");
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
