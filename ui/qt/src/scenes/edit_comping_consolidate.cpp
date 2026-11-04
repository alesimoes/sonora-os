// SPDX-License-Identifier: GPL-3.0-or-later
//
// Consolidate (ADR-0031 §6.1): the comp selected on the lead vocal and the
// engine rendering it, the progress held under the window with a Cancel, the
// way a failure would be held with a Dismiss.
#include "comping_scene.h"

namespace sonora::ui {
namespace {

using namespace comping_scene;

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-comping-consolidate");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            if (!setup(c)) return;
            c.model.invokeWith(QStringLiteral("comp.toggle_quick_swipe"), -1, QString());
            swipe(c, 0, 4, 9);
            swipe(c, 1, 9, 13);
            // The range across the comp on the track's own row, then the
            // key's command; the engine's answer arrives as events, shown
            // after the demo engine has finished its own.
            auto& ctl = c.session.controller();
            const auto x = [&](int n) { return ctl.view().x_of(n * bar(c)); };
            const double y = ctl.view().y_of_track(kVox) + 12;
            c.session.press(x(4), y);
            c.session.drag(x(4) + 8, y);
            c.session.drag(x(13), y);
            c.session.release(x(13), y);
            c.model.invokeWith(QStringLiteral("edit.consolidate"), -1, QString());
            QTimer::singleShot(1500, [c]() {
                c.model.showConsolidate(QStringLiteral("Consolidating… 42 %"), false, true);
            });
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
