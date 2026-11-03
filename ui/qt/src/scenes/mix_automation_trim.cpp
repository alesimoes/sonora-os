// SPDX-License-Identifier: GPL-3.0-or-later
//
// The trim fader (ADR-0027 §8 slice 5). Strip 1 is in Write/Trim and its
// trim fader was ridden to about +2.5 dB, which plays until it moves again:
// the cap, its line and the readout ("trim +2.5") in the trim colour, TRIM
// over the scale, the volume left where the document has it. Strip 2 is in
// Trim with a trim lane, released: the lane plays, so the readout says
// "trim lane" and the cap is dimmed. Strip 3 is in Trim with no lane, at 0
// dB of trim. Strip 4 stays in Read for comparison.
#include <QQuickItem>
#include <QTimer>
#include <cstdio>
#include <string>

#include "mix_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-automation-trim");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        auto* mixer = c.view.rootObject()->property("mixer").value<MixModel*>();
        if (mixer == nullptr || c.session.trackCount() < 4) {
            std::fprintf(stderr, "sonora-ui: trim scene: no mixer or too few tracks\n");
            return;
        }
        EditController& edit = c.session.controller();
        std::string why;
        const model::AutomationMode modes[] = {model::AutomationMode::WriteTrim,
                                               model::AutomationMode::Trim,
                                               model::AutomationMode::Trim};
        for (int t = 0; t < 3; ++t) {
            if (!edit.set_automation_mode(t, modes[t], why))
                std::fprintf(stderr, "sonora-ui: automation mode: %s\n", why.c_str());
        }
        model::EditAutomation lane;
        lane.track = edit.snapshot().edit->tracks[1]->id;
        lane.param = model::AutomationParam::VolumeTrim;
        lane.from = 0;
        lane.to = model::kMaxInteger;
        lane.points = {{0, -3.0f}, {480000, 2.0f}};
        if (!edit.document().submit(lane, why))
            std::fprintf(stderr, "sonora-ui: trim lane: %s\n", why.c_str());
        // The hand on strip 1's trim fader, up and let go, once the engine
        // has answered the mode (before that the fader is still the volume
        // fader); the travel is a nominal one, so the step is the taper's.
        QTimer::singleShot(300, [mixer]() {
            mixer->beginRide(0);
            mixer->ride(0, -12, 300, false);
            mixer->endRide(0);
        });
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
