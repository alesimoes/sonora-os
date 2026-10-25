// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Tempo and Meter rulers on the demo's tempo map (120 4/4, 96 from bar
// 17, 3/4 from bar 25), the insertion point typed into the main counter at
// the meter change.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-conductor");
    s.tempo_map = true;
    s.take = false;
    s.apply = [](ShotContext& c) { c.session.enterCounter(QStringLiteral("25|1|000")); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
