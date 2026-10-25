// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the plugin chooser open on Lead Vox's fourth slot.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-browser");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) { c.inserts.openBrowser(14, 3); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
