// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snare's region repeated three times through the dialog's command,
// then Alt+R's dialog open again on it, offering the count just used.
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-repeat");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        if (c.playlist == nullptr || c.playlist->regions.size() <= 1) return;
        // An id, not the playlist: the repeat replaces the snapshot it is in.
        const auto snare = c.playlist->regions[1].id;
        c.session.controller().select_region(snare);
        if (!c.model.invokeWith(QStringLiteral("edit.repeat"), -1, QStringLiteral("3")))
            std::fprintf(stderr, "sonora-ui: repeat: %s\n", qPrintable(c.model.statusText()));
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        c.session.controller().select_region(snare);
        c.model.invoke(QStringLiteral("edit.repeat"), -1);
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
