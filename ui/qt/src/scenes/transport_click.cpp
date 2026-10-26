// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window with the click and the count-off turned on, through
// their commands as keypad 7 and 8 send them (ADR-0020): both buttons lit
// by the engine's state block, not by the press.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-click");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) {
        c.transport.setOpen(true);
        c.model.invoke(QStringLiteral("transport.toggle_click"), -1);
        c.model.invoke(QStringLiteral("transport.toggle_count_off"), -1);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
