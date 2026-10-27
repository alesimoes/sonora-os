// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bus chooser (ADR-0022) on the first track's empty send C: the delay
// bus is offered; the reverb, which its send A already feeds, is dimmed
// with the reason.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-send-chooser");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) { c.sends.openChooser(0, 2); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
