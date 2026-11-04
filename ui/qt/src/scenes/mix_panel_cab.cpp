// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with a cabinet's panel open: choices for the cabinet and
// microphone, knobs grouped by section, input and output levels (ADR-0057).
#include "telemetry_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-cab");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c) {
        // The fake cabinet is catalog entry 5.
        show_plugin_panel(c, 5, 0, 5, [](double t) {
            api::PluginTelemetry f;
            f.meter_mask = 0b11;
            const float in = wander(t, -18.0, 4.0, 1.1);
            f.meters = {in, in - 3.5f};
            return f;
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
