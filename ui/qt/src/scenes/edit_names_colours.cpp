// SPDX-License-Identifier: GPL-3.0-or-later
//
// Names and colours (ADR-0023, update of 2026-11-02) in the Edit window:
// the kick and the hi-hat (track index 2) recoloured from the palette, the snare's
// first region renamed "Snare intro" and given a colour of its own, the
// snare moved down a row keeping its colour, and the colour palette open
// on the selected track, its colour ringed.
#include <QQuickItem>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

bool run(ShotContext& c, const char* command, int target, const char* argument) {
    if (c.model.invokeWith(QString::fromLatin1(command), target, QString::fromUtf8(argument)))
        return true;
    std::fprintf(stderr, "sonora-ui: %s: %s\n", command, qPrintable(c.model.statusText()));
    return false;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-names-colours");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            if (!run(c, "track.colour", 0, "9") || !run(c, "track.colour", 2, "13")) return;
            if (c.playlist == nullptr || c.playlist->regions.empty()) return;
            c.session.controller().select_region(c.playlist->regions.front().id);
            if (!run(c, "region.rename", -1, "Snare intro") || !run(c, "region.colour", -1, "12"))
                return;
            if (!run(c, "track.move_down", c.track, "")) return;
            c.session.selectTrack(c.track + 1);
            run(c, "track.colour", c.track + 1, "");
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
