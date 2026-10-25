// SPDX-License-Identifier: GPL-3.0-or-later
//
// A range on the first three tracks copied and pasted after their last
// region, through the keys' commands: the pasted range stays selected
// across the three lanes.
#include <algorithm>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-multitrack");
    s.take = false;
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 2 || c.session.trackCount() < 3) return;
        EditController& edit = c.session.controller();
        const auto from = p->regions[1].position, to = p->regions[2].end();
        edit.select_range(0, from, to, 3);
        c.model.invoke(QStringLiteral("edit.copy"), -1);
        model::Frames after = to;
        for (int i = 0; i < 3; ++i) {
            const auto* lane = active_playlist(edit.snapshot().edit->tracks[i].get());
            if (lane && !lane->regions.empty()) after = std::max(after, lane->regions.back().end());
        }
        const auto at = after + (to - from) / 4;
        edit.select_range(0, at, at, 3);
        c.model.invoke(QStringLiteral("edit.paste"), -1);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        if (edit.selection().tracks != 3)
            std::fprintf(stderr, "sonora-ui: multitrack: %s\n", qPrintable(c.model.statusText()));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
