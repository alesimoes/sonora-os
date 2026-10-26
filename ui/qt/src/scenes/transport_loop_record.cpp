// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window in the Pro Tools profile with Loop Record on over the
// Edit selection and Loop Playback off, through transport.toggle_loop_record
// as Option+L sends it: Loop Rec lit and Loop not, two modes apart, and the
// range shown from the document the engine changed.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-loop-record");
    s.args = QStringLiteral("--keymap keymaps/protools.json");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->regions.size() <= 2) return;
        c.session.controller().select_range(c.track, p->regions[1].position, p->regions[2].end());
        c.model.invoke(QStringLiteral("transport.toggle_loop_record"), -1);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
