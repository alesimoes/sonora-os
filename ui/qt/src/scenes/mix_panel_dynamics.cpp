// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the kick's compressor's panel open and live: sections,
// the transfer curve, gain reduction and the levels (ADR-0057).
#include "telemetry_scene.h"

namespace sonora::ui {
namespace {

float transfer(double x, double thr, double ratio, double knee) {
    if (2.0 * (x - thr) < -knee) return static_cast<float>(x);
    if (2.0 * std::fabs(x - thr) <= knee) {
        const double d = x - thr + knee / 2.0;
        return static_cast<float>(x + (1.0 / ratio - 1.0) * d * d / (2.0 * knee));
    }
    return static_cast<float>(thr + (x - thr) / ratio);
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-dynamics");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c) {
        show_plugin_panel(c, 0, 0, -1, [](double t) {
            api::PluginTelemetry f;
            f.meter_mask = 0b111;
            const float gr = std::max(0.0f, wander(t, 5.5f, 2.6, 2.1));
            const float in = wander(t, -13.0, 3.0, 1.3);
            f.meters = {gr, in, in - gr + 2.0f};
            std::vector<float> curve(128);
            for (std::size_t i = 0; i < curve.size(); ++i)
                curve[i] = transfer(-60.0 + 60.0 * static_cast<double>(i) / 127.0, -24.0, 4.0, 8.0);
            f.curves = {curve};
            return f;
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
