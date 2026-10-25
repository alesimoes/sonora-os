// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snare's region copied and pasted into a range on four tracks after the
// session's end: one track's clipboard fills every selected track, as in
// Pro Tools.
#include <algorithm>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-fill");
    s.take = false;
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 1 || c.session.trackCount() < 5) return;
        EditController& edit = c.session.controller();
        const auto snare = p->regions[1];
        edit.select_region(snare.id);
        c.model.invoke(QStringLiteral("edit.copy"), -1);
        model::Frames after = 0;
        for (int i = 1; i < 5; ++i) {
            const auto* lane = active_playlist(edit.snapshot().edit->tracks[i].get());
            if (lane && !lane->regions.empty()) after = std::max(after, lane->regions.back().end());
        }
        const auto at = after + snare.length / 4;
        edit.select_range(c.track, at, at + snare.length, 4);
        c.model.invoke(QStringLiteral("edit.paste"), -1);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        if (edit.selection().tracks != 4)
            std::fprintf(stderr, "sonora-ui: fill: %s\n", qPrintable(c.model.statusText()));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
