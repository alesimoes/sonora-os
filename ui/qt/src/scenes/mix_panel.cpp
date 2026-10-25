// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the kick's compressor's parameter panel open.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) { c.inserts.openPanel(0, 0); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
