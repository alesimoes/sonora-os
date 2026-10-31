// SPDX-License-Identifier: GPL-3.0-or-later
//
// Five markers through marker.add, as the New Marker dialog sends them, one
// of them a selection marker over a range, then the Memory Locations window
// open on them, the selection marker recalled. The main counter is on
// Timecode, so the LENGTH column reads as timecode too, not min:sec.
#include <QTimer>
#include <algorithm>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

// The engine answers the demo session's Load a moment after the window is
// up, and a request before that is refused ("still opening"): the first
// marker is asked again until it goes, within the capture's wait.
void build(ShotContext c, int tries);

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-markers-timecode");
    s.take = false;
    s.apply = [](ShotContext& c) { build(c, 0); };
    return s;
}()};

void build(ShotContext c, int tries) {
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
    bool first = true;
    for (const auto& m : marks) {
        const auto at = static_cast<model::Frames>(end * m.at);
        if (m.length > 0) {
            edit.select_range(std::max(0, edit.selection().track), at,
                              at + static_cast<model::Frames>(end * m.length));
        } else {
            edit.set_insertion(at);
        }
        if (c.model.invokeWith(QStringLiteral("marker.add"), -1, QString::fromUtf8(m.name))) {
            first = false;
            continue;
        }
        if (first && tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
            return;
        }
        std::fprintf(stderr, "sonora-ui: marker: %s\n", qPrintable(c.model.statusText()));
    }
    c.session.cycleMainCounter();  // Min:Secs
    c.session.cycleMainCounter();  // Timecode
    c.session.recallMarker(2);
    c.model.invoke(QStringLiteral("window.memory_locations"), -1);
}

}  // namespace
}  // namespace sonora::ui
