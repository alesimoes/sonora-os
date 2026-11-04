// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tempo track under the Tempo and Meter rulers, on the demo's tempo map
// (120 4/4, 96 from bar 17, 3/4 from bar 25) with a ramp from 120 down to 84
// over bars 5 to 13: a slope on the line, a handle at each step, the bar
// lines of the Bars|Beats ruler moving with it.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-tempo-track");
    s.tempo_map = true;
    s.take = false;
    s.apply = [](ShotContext& c) {
        const model::Frames bar120 = 2 * static_cast<model::Frames>(
                                             c.session.controller().snapshot().settings.sample_rate);
        std::string why;
        c.model.invoke(QStringLiteral("tempo.track"), -1);
        c.session.controller().ramp_tempo(4 * bar120, 12 * bar120, 120.0f, 84.0f, why);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
