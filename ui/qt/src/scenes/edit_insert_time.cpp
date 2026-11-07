// SPDX-License-Identifier: GPL-3.0-or-later
//
// Insert Time: a range selected on the first track, two bars wide, opened
// where it starts. The regions after it (and the markers and automation
// there) move up by the range's length in one undo step; the title bar
// names the step, "Insert Time", in the Edit menu's Undo.
#include <cstdio>
#include <string>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-insert-time");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        if (c.playlist == nullptr || c.playlist->regions.size() < 2) return;
        EditController& edit = c.session.controller();
        const auto at = c.playlist->regions[0].end();
        const auto length = (c.playlist->regions[1].position - at) + c.playlist->regions[1].length / 2;
        edit.select_range(c.track, at, at + length, 1);
        c.model.invoke(QStringLiteral("edit.insert_time"), -1);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
