// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the cabinet's panel open in the generic editor: Output
// and Mix as knobs, and the two response switches, Trim (on) and Normalise
// (off), as toggles.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-cab");
    s.window = QStringLiteral("mix");
    s.cabinet = true;
    s.setup = [](ShotContext& c) { c.inserts.openPanel(4, 0); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
