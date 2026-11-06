// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window with clip gain on the snare's first region: a level of
// -3 dB and a deep dip in the middle of the clip, the line and its handles drawn
// over the waveform, the level in the clip's name.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-clip-gain");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            const auto* p = c.playlist;
            if (p == nullptr || p->regions.empty()) return;
            const model::Region& r = p->regions.front();
            auto& ctl = c.session.controller();
            std::string why;
            ctl.set_insertion(r.position + r.length / 2);
            c.model.invoke(QStringLiteral("zoom.in"), -1);
            if (!ctl.select_region(r.id)) return;
            ctl.set_clip_gain(-3.0f, why);
            ctl.set_gain_point(r.position + r.length / 2, -24.0f, why);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
