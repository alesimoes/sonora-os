// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the cabinet's panel open in the generic editor: Output
// and Mix as knobs, the two response switches, Trim (on) and Normalise (off),
// as toggles, and the input and output levels live (ADR-0057).
#include "telemetry_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-cab");
    s.window = QStringLiteral("mix");
    s.cabinet = true;
    s.apply = [](ShotContext& c) {
        // Track 4 already carries the cabinet (ShotScene::cabinet).
        show_plugin_panel(c, 4, 0, -1, [](double t) {
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
