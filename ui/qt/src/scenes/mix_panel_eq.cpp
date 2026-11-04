// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with an EQ's panel open and live: a section per band and the
// magnitude response the bands make (ADR-0057). The curve is drawn from the
// fake EQ's default settings with a bump per band, for the picture; the real
// one is the plugin's own response, computed in the plugin.
#include "telemetry_scene.h"

namespace sonora::ui {
namespace {

float response_db(double f) {
    const auto bump = [f](double centre, double gain, double q) {
        const double d = std::log(f / centre) * q;
        return gain * std::exp(-d * d);
    };
    const auto shelf = [f](double centre, double gain, bool high) {
        const double x = std::log(f / centre) * 2.2;
        const double s = 1.0 / (1.0 + std::exp(high ? -x : x));
        return gain * s;
    };
    return static_cast<float>(shelf(100.0, 3.0, false) + bump(400.0, -4.0, 1.2) + bump(3200.0, 5.0, 0.9) +
                              shelf(10000.0, 2.0, true));
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-panel-eq");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c) {
        // The fake EQ is catalog entry 4.
        show_plugin_panel(c, 4, 0, 4, [](double t) {
            api::PluginTelemetry f;
            f.meter_mask = 0b11;
            const float in = wander(t, -16.0, 3.0, 1.7);
            f.meters = {in, in + 1.5f};
            std::vector<float> curve(128);
            for (std::size_t i = 0; i < curve.size(); ++i)
                curve[i] = response_db(20.0 * std::pow(1000.0, static_cast<double>(i) / 127.0));
            f.curves = {curve};
            return f;
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
