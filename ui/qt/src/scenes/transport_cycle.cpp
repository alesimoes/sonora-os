// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window in the Logic profile: one Cycle button, lit by
// transport.toggle_cycle as C sends it, over the cycle area already set and
// not the Edit selection.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-cycle");
    s.args = QStringLiteral("--keymap keymaps/logic.json");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 2) return;
        // A cycle area at the second region, set and left off...
        c.session.controller().select_range(c.track, p->regions[1].position, p->regions[1].end());
        c.model.invoke(QStringLiteral("transport.toggle_loop"), -1);
        c.model.invoke(QStringLiteral("transport.toggle_loop"), -1);
        // ...then another selection, and Cycle keeps the area.
        c.session.controller().select_range(c.track, p->regions[2].position, p->regions[2].end());
        c.model.invoke(QStringLiteral("transport.toggle_cycle"), -1);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
