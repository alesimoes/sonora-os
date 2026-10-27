// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's sends (ADR-0022): the demo's aux buses after the tracks,
// and the send window open on the fourth track's pre-fader send to the
// delay bus.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-sends");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) { c.sends.openWindow(3, 1); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
