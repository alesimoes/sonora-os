// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample view (ADR-0096 §3): the snare's first clip at 4 pixels a sample,
// its samples a line with a dot on each, the Pencil in hand and a stroke
// drawn over them, previewed where the release will write it. The demo has
// no audio on disk, so the samples are main.cpp's stand-in sine.
#include <cmath>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-sample-view");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            if (c.playlist == nullptr || c.playlist->regions.empty()) return;
            auto& ctl = c.session.controller();
            const model::Region r = c.playlist->regions.front();
            ctl.clear_selection();
            ctl.set_track_height(150);
            // Past kSampleDotsMaxFpp, so each sample is a dot as well.
            ctl.zoom_by(0.25 / ctl.view().fpp, 0);
            ctl.scroll_to_frame(static_cast<double>(r.position) + 40.0);
            c.model.invokeWith(QStringLiteral("tool.pencil"), -1, QString());
            const SampleLane lane =
                sample_lane(ctl.view().y_of_track(c.track), ctl.view().track_height,
                            static_cast<int>(r.channels.size()), 0);
            const auto y = [&](double level) { return lane.mid() - level * lane.height / 2; };
            const double w = ctl.view().width;
            // A swell drawn over the sine, held before the release.
            const double x0 = 0.35 * w, x1 = 0.65 * w;
            c.session.press(x0, y(0.0));
            for (int i = 1; i <= 24; ++i) {
                const double t = i / 24.0;
                c.session.drag(x0 + t * (x1 - x0), y(0.8 * std::sin(3.14159265358979 * t)));
            }
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
