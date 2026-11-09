// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sections in Memory Locations (ADR-0116): two selection markers over the
// demo's first regions, the first copied to the insertion point after the
// second, so the window shows the sections, the destination and the held
// message of what the copy left where it was.
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void build(ShotContext& c) {
    EditController& edit = c.session.controller();
    const auto* p = c.playlist;
    if (p == nullptr || p->regions.size() < 3) {
        std::fprintf(stderr, "sonora-ui: edit-arrangement: the demo has too few regions\n");
        return;
    }
    const struct {
        const char* name;
        std::size_t region;
    } sections[] = {{"Verse", 0}, {"Chorus", 1}};
    for (const auto& s : sections) {
        const model::Region& r = p->regions[s.region];
        edit.select_range(c.track, r.position, r.end());
        if (!c.model.invokeWith(QStringLiteral("marker.add"), -1, QString::fromUtf8(s.name)))
            std::fprintf(stderr, "sonora-ui: arrangement: %s\n", qPrintable(c.model.statusText()));
    }
    edit.locate_to(p->regions[1].end());
    c.session.arrangementApply(0, false);
    c.model.invoke(QStringLiteral("window.memory_locations"), -1);
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-arrangement");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) { build(c); }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
