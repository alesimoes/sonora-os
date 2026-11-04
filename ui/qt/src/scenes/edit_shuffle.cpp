// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shuffle mode (F1): the second region cut, and the regions after it moved
// up by its length in the same undo step, so the playlist closes over the
// gap Slip would have left. The title bar names the mode.
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-shuffle");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        if (c.playlist == nullptr || c.playlist->regions.size() <= 2) return;
        // An id, not the playlist: the cut replaces the snapshot it is in.
        const auto second = c.playlist->regions[1].id;
        c.model.invoke(QStringLiteral("edit.mode.shuffle"), -1);
        c.session.controller().select_region(second);
        c.model.invoke(QStringLiteral("edit.cut"), -1);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
