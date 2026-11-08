// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Sample Edit panel (ADR-0096): at 2 frames a pixel the snare's first
// clip shows its samples as a line, a range is selected inside it with the
// Selector, and the panel under the timeline offers the sample.* edits on
// it, live because the range lies inside one clip.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-sample-panel");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            if (c.playlist == nullptr || c.playlist->regions.empty()) return;
            auto& ctl = c.session.controller();
            const model::Region r = c.playlist->regions.front();
            ctl.set_track_height(120);
            ctl.zoom_by(2.0 / ctl.view().fpp, 0);
            ctl.scroll_to_frame(static_cast<double>(r.position) + 100.0);
            c.model.invokeWith(QStringLiteral("tool.select"), -1, QString());
            const double w = ctl.view().width;
            const model::Frames a = ctl.view().frame_at(0.40 * w);
            const model::Frames b = std::min(ctl.view().frame_at(0.60 * w), r.end());
            ctl.select_range(c.track, a, b);
            // Through the model, so the panel and the selection fields follow.
            c.session.drag(0, 0);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
