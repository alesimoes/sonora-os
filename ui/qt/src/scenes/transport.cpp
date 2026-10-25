// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window open over the Edit window, on the demo session with
// a tempo map.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport");
    s.tempo_map = true;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
