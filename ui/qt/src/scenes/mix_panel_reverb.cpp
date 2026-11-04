// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with a reverb's panel open: the space as choices, decay and
// tone in their sections, levels at the top (ADR-0057).
#include "telemetry_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-reverb");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c) {
        // The fake reverb is catalog entry 6.
        show_plugin_panel(c, 6, 0, 6, [](double t) {
            api::PluginTelemetry f;
            f.meter_mask = 0b11;
            const float in = wander(t, -20.0, 5.0, 0.9);
            f.meters = {in, in - 9.0f};
            return f;
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
