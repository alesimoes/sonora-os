// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window with a clip selected and its loop handle dragged 2.4 lengths
// past the end: two ghost repeats show where the drop will lay them.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("clip-loop-handle");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            auto& ctl = c.session.controller();
            const auto& tracks = ctl.snapshot().edit->tracks;
            if (tracks.empty()) return;
            const model::Playlist* p = active_playlist(tracks[0].get());
            if (p == nullptr || p->regions.empty()) return;
            const model::Region& r = p->regions.front();
            if (!ctl.select_region(r.id)) return;
            const double low = ctl.view().y_of_track(0) + ctl.view().row_h(0) * 0.9;
            const double x0 = ctl.view().x_of(r.end()) - 2;
            const double x1 = ctl.view().x_of(r.end() + r.length * 12 / 5);
            std::string why;
            if (ctl.press(x0, low, why)) ctl.move(x1, low);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
