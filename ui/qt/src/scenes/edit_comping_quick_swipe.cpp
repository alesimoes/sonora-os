// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quick Swipe (ADR-0031 §3), Logic's way: three drags across the take lanes,
// each copied as the button comes up with a short crossfade at its joins.
#include "comping_scene.h"

namespace sonora::ui {
namespace {

using namespace comping_scene;

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-comping-quick-swipe");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            if (!setup(c)) return;
            c.model.invokeWith(QStringLiteral("comp.toggle_quick_swipe"), -1, QString());
            swipe(c, 0, 4, 9);
            swipe(c, 1, 9, 13);
            swipe(c, 0, 13, 17);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
