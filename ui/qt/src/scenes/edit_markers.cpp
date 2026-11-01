// SPDX-License-Identifier: GPL-3.0-or-later
//
// Five markers through marker.add, as the New Marker dialog sends them, one
// of them a selection marker over a range, then the Memory Locations window
// open on them, the selection marker recalled. Two pictures: the main counter
// on its default, and on Timecode, where the LENGTH column reads as timecode
// too, not min:sec.
#include <algorithm>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void build(ShotContext& c, int counter_cycles) {
    EditController& edit = c.session.controller();
    const double end = static_cast<double>(edit.session_end());
    const struct {
        const char* name;
        double at, length;
    } marks[] = {{"Intro", 0.0, 0.0},
                 {"Verse 1", 0.12, 0.0},
                 {"Chorus", 0.36, 0.18},
                 {"Verse 2", 0.62, 0.0},
                 {"Outro", 0.86, 0.0}};
    for (const auto& m : marks) {
        const auto at = static_cast<model::Frames>(end * m.at);
        if (m.length > 0) {
            edit.select_range(std::max(0, edit.selection().track), at,
                              at + static_cast<model::Frames>(end * m.length));
        } else {
            edit.set_insertion(at);
        }
        if (!c.model.invokeWith(QStringLiteral("marker.add"), -1, QString::fromUtf8(m.name)))
            std::fprintf(stderr, "sonora-ui: marker: %s\n", qPrintable(c.model.statusText()));
    }
    for (int i = 0; i < counter_cycles; ++i) c.session.cycleMainCounter();
    c.session.recallMarker(2);
    c.model.invoke(QStringLiteral("window.memory_locations"), -1);
}

ShotScene markers_scene(const char* name, int counter_cycles) {
    ShotScene s;
    s.name = QString::fromLatin1(name);
    s.take = false;
    s.apply = [counter_cycles](ShotContext& c0) {
        when_open(c0, [counter_cycles](ShotContext& c) { build(c, counter_cycles); });
    };
    return s;
}

// Two cycles from the default counter reach Timecode (Min:Secs, then Timecode).
const ShotRegistrar registrar{markers_scene("edit-markers", 0)};
const ShotRegistrar registrar_timecode{markers_scene("edit-markers-timecode", 2)};

}  // namespace
}  // namespace sonora::ui
