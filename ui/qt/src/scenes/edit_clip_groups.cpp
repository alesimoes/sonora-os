// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window with a clip group and a clip loop: the first track's first two
// clips grouped (a teal bar along their foot, one move for both), and the second
// track's last clip looped twice, its repeats named and barred in violet.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-clip-groups");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            auto& ctl = c.session.controller();
            const auto& tracks = ctl.snapshot().edit->tracks;
            if (tracks.size() < 2) return;
            std::string why;
            const model::Playlist* a = active_playlist(tracks[0].get());
            const model::Playlist* b = active_playlist(tracks[1].get());
            if (a == nullptr || b == nullptr || a->regions.size() < 2 || b->regions.empty()) return;
            const model::Id looped = b->regions.back().id;
            ctl.select_range(0, a->regions[0].position, a->regions[1].end());
            ctl.group_clips(why);
            if (ctl.select_region(looped)) ctl.loop_clip(2, why);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
