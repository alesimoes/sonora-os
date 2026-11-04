// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the loudness panel beside the master: the demo engine
// measures no loudness, so the panel is given a reading (LoudnessModel::
// showDemo): EBU R128 chosen as the target, the integrated value inside its
// tolerance and the true peak under the ceiling.
#include "loudness_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-loudness");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        auto* m = c.view.rootObject()->property("loudness").value<LoudnessModel*>();
        if (m != nullptr) m->showDemo();
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
