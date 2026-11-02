// SPDX-License-Identifier: GPL-3.0-or-later
//
// Names and colours (ADR-0023, update of 2026-11-02) in the Mix window:
// strips recoloured from the palette wear their colour on the cap, a track
// renamed shows it on the nameplate, and Rename Track is open on the
// selected strip, as a double-click on its nameplate opens it.
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
    s.name = QStringLiteral("mix-names-colours");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            const char* colours[] = {"9", "9", "13", "13", "11", "11"};
            for (int i = 0; i < 6; ++i)
                if (!run(c, "track.colour", i, colours[i])) return;
            if (!run(c, "track.rename", 0, "Kick In")) return;
            run(c, "track.rename", c.track, "");
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
