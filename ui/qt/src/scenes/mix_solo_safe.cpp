// SPDX-License-Identifier: GPL-3.0-or-later
//
// A vocal soloed over a session whose room mic and talkback are solo safe:
// those two stay audible and say so, with recessed solo buttons and a
// padlock, and the rest are outlined as the solo silences them.
#include <cstdio>
#include <string>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-solo-safe");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) {
        EditController& edit = c.session.controller();
        std::string why;
        for (const int t : {7, 17}) {
            if (t < c.session.trackCount() && !edit.toggle_solo_safe(t, why))
                std::fprintf(stderr, "sonora-ui: solo safe: %s\n", why.c_str());
        }
        if (14 < c.session.trackCount()) c.model.invoke(QStringLiteral("track.toggle_solo"), 14);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
