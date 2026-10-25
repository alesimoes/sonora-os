// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the demo's tempo map, the snare separated where 96 bpm starts and the
// part after it nudged forward two beats on the 1/4 grid: the gap is two
// 96 bpm beats and the region lands on 17|3, not on 120 bpm steps.
#include <cstdio>
#include <string>

#include "screenshot_scene.h"
#include "time_format.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-nudge");
    s.take = false;
    s.tempo_map = true;
    s.apply = [](ShotContext& c) {
        EditController& edit = c.session.controller();
        const auto& tempo = edit.snapshot().settings.tempo;
        const auto change = tempo.size() > 1 ? tempo[1].at : 0;
        std::string why;
        edit.select_track(c.track);
        edit.set_insertion(change);
        if (!edit.separate(why)) std::fprintf(stderr, "sonora-ui: nudge: %s\n", why.c_str());
        model::Id pick = kNoId;
        if (const auto* pl = active_playlist(edit.snapshot().edit->tracks[c.track].get())) {
            for (const auto& r : pl->regions) {
                if (r.position == change) pick = r.id;
            }
        }
        if (pick != kNoId && edit.select_region(pick)) {
            edit.set_grid(GridValue::Beat);
            for (int i = 0; i < 2; ++i) c.model.invoke(QStringLiteral("nudge.forward"), -1);
            edit.set_insertion(change);
            for (int i = 0; i < 3; ++i) c.model.invoke(QStringLiteral("zoom.in"), -1);
        } else {
            std::fprintf(stderr, "sonora-ui: nudge: no region at the tempo change\n");
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
