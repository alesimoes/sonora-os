// SPDX-License-Identifier: GPL-3.0-or-later
//
// A range selected across three tracks with Link Timeline and Edit Selection
// on: the timeline's range is the same bars (ADR-0167).
#include <cstdio>

#include "screenshot_scene.h"
#include "selection_link.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("selection-link");
    s.take = false;
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 1) return;
        static SelectionLink link;
        EditController& edit = c.session.controller();
        edit.set_link(&link);
        const auto& r = p->regions[1];
        edit.select_range(c.track, r.start, r.end(), 3);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        if (link.timeline().end != r.end())
            std::fprintf(stderr, "sonora-ui: selection-link: range not mirrored\n");
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
