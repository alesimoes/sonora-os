// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the comping scenes share: the lead vocal given two more takes on lanes
// of their own, the lanes shown, and a swipe made the way a hand makes it, with
// the mouse on the lane, so the picture is of the gesture and not of a model
// edit that no gesture makes.
#pragma once

#include <QTimer>
#include <cstdio>
#include <string>

#include "comping_demo.h"
#include "screenshot_scene.h"

namespace sonora::ui::comping_scene {

constexpr int kVox = 14;   // the demo's lead vocal

inline model::Frames bar(const ShotContext& c) {
    return static_cast<model::Frames>(c.session.controller().snapshot().settings.sample_rate) * 2;
}

// Takes 2 and 3 over bars 4 to 18, the lanes open, the track at the top.
inline bool setup(ShotContext& c) {
    auto& ctl = c.session.controller();
    std::string why;
    if (!add_demo_takes(ctl.document(), kVox, 4, 14, why)) {
        std::fprintf(stderr, "sonora-ui: scene: takes: %s\n", why.c_str());
        return false;
    }
    ctl.select_track(kVox);
    c.model.invokeWith(QStringLiteral("comp.toggle_lanes"), kVox, QString());
    ctl.scroll_by(0, ctl.view().y_of_track(kVox) - 24);
    c.session.wheel(0, 0, 0, false, false);   // a sync, so the picture follows
    return true;
}

// A drag across lane `lane` from bar `a` to bar `b`.
inline void swipe(ShotContext& c, int lane, int a, int b) {
    auto& ctl = c.session.controller();
    const auto x = [&](int n) { return ctl.view().x_of(n * bar(c)); };
    const double y = ctl.view().y_of_complane(kVox, lane) + 16;
    c.session.press(x(a), y);
    c.session.drag(x(a) + 8, y);
    c.session.drag(x(b), y);
    c.session.release(x(b), y);
}

inline void copy_to_main(ShotContext& c) {
    c.model.invokeWith(QStringLiteral("comp.copy_to_main"), -1, QString());
}

}  // namespace sonora::ui::comping_scene
