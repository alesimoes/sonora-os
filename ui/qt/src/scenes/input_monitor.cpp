// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two tracks in Input mode (Pro Tools' TrackInput), their "I" buttons lit
// in the input colour, in the Mix window and in the Edit window's headers.
// Each is turned on by track.toggle_input_monitor, the command the key and
// the button send.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void two_inputs(ShotContext& c) {
    for (const int t : {0, 2}) {
        if (t < c.session.trackCount())
            c.model.invoke(QStringLiteral("track.toggle_input_monitor"), t);
    }
}

const ShotRegistrar mix{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-input-monitor");
    s.window = QStringLiteral("mix");
    s.setup = two_inputs;
    return s;
}()};

const ShotRegistrar edit{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-input-monitor");
    s.window = QStringLiteral("edit");
    s.setup = two_inputs;
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
