// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one capture that keeps the software-rendering banner: the headless rig
// has no GPU, and the product says so. Every other capture hides it.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-software-banner");
    s.software_banner = true;
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
