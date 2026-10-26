// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window with loop playback turned on over the Edit
// selection, through transport.toggle_loop as Ctrl+Shift+L and C send it:
// the Loop button lit and the range shown from the document the engine
// changed, not from the press.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-loop");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 2) return;
        c.session.controller().select_range(c.track, p->regions[1].position, p->regions[2].end());
        c.model.invoke(QStringLiteral("transport.toggle_loop"), -1);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
