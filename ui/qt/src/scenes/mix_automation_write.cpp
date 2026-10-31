// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strips in Touch, Latch and Write with the selector closed: the button is
// lit amber while the strip can write on a touch, and red in Write.
#include <cstdio>
#include <string>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-automation-write");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.setup = [](ShotContext& c) {
        EditController& edit = c.session.controller();
        std::string why;
        const model::AutomationMode modes[] = {model::AutomationMode::Write,
                                               model::AutomationMode::Touch,
                                               model::AutomationMode::Touch,
                                               model::AutomationMode::Latch};
        for (int t = 0; t < 4 && t < c.session.trackCount(); ++t) {
            if (!edit.set_automation_mode(t, modes[t], why))
                std::fprintf(stderr, "sonora-ui: automation mode: %s\n", why.c_str());
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
