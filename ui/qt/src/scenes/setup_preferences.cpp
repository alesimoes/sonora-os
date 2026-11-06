// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > Preferences: the window opened through window.preferences, over
// the Edit window, with the audio format and folders shown and the counter,
// edit mode and grid offered.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("preferences");
    s.take = false;
    s.apply = [](ShotContext& c) { c.model.invoke(QStringLiteral("window.preferences"), -1); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
