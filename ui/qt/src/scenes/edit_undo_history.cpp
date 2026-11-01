// SPDX-License-Identifier: GPL-3.0-or-later
//
// A few edits through the keys' commands, the last one undone, and the Undo
// History window open on them: done steps, the line, and the undone step in
// italics below it.
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-undo-history");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        if (c.playlist == nullptr || c.playlist->regions.size() <= 1) return;
        EditController& edit = c.session.controller();
        const auto snare = c.playlist->regions[1].id;
        edit.select_region(snare);
        c.model.invoke(QStringLiteral("edit.duplicate"), -1);
        edit.select_region(snare);
        if (!c.model.invokeWith(QStringLiteral("edit.repeat"), -1, QStringLiteral("2")))
            std::fprintf(stderr, "sonora-ui: repeat: %s\n", qPrintable(c.model.statusText()));
        const auto* first = active_playlist(edit.snapshot().edit->tracks[0].get());
        if (first && !first->regions.empty()) {
            const auto kick = first->regions[0].id;
            edit.select_region(kick);
            c.model.invoke(QStringLiteral("edit.duplicate"), -1);
            edit.select_region(kick);
            c.model.invoke(QStringLiteral("edit.clear"), -1);
        }
        c.model.invoke(QStringLiteral("edit.undo"), -1);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        c.model.invoke(QStringLiteral("window.undo_history"), -1);
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
