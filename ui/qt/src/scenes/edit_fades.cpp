// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window zoomed in about the snare's crossfade, through the zoom
// keys' commands, so the fades near it are wide enough to read.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-fades");
    s.take = false;
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        if (p == nullptr || p->crossfades.size() != 1) return;
        const auto& x = p->crossfades[0];
        const model::Region* right = nullptr;
        for (const auto& r : p->regions) {
            if (r.id == x.right) right = &r;
        }
        if (right != nullptr) {
            c.session.controller().set_insertion(right->position);
            for (int i = 0; i < 2; ++i) c.model.invoke(QStringLiteral("zoom.in"), -1);
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
