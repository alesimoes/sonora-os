// SPDX-License-Identifier: GPL-3.0-or-later
//
// Show/Hide Tracks (ADR-0106): the first track Large, the third Small and
// the fifth hidden, each through the command a key would send, with the
// Tracks list open over the Edit window. Hiding is what the list's buttons
// do too; the row heights below the hidden track close up.
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

bool step(ShotContext& c, const char* command, int target, const QString& argument = {}) {
    if (c.model.invokeWith(QString::fromLatin1(command), target, argument)) return true;
    std::fprintf(stderr, "sonora-ui: %s: %s\n", command, qPrintable(c.model.statusText()));
    return false;
}

void build(ShotContext& c) {
    if (!step(c, "track.height", 0, QStringLiteral("large"))) return;
    if (!step(c, "track.height", 2, QStringLiteral("small"))) return;
    if (!step(c, "track.hide", 5)) return;
    step(c, "window.tracks_list", -1);
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("track-list");
    s.take = false;
    s.apply = [](ShotContext& c) {
        if (c.session.trackCount() < 6) return;
        when_open(c, build);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
