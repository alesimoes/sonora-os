// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Step Sequencer on the demo: a four-on-the-floor kick, a backbeat snare,
// off-beat hats, a lane with nothing assigned, swing at 58 %, and the snare's
// second hit selected so its velocity, probability and pitch show in the lane.
// Opened the way the operator does, through window.step_sequencer.
#include <QQuickItem>

#include "screenshot_scene.h"
#include "step_sequencer_model.h"

namespace sonora::ui {
namespace {

void fill(StepSequencerModel& m) {
    for (int r = 0; r < 3; ++r) m.setRowSource(r, static_cast<stepseq::SourceId>(r + 1));
    for (int s = 0; s < 16; s += 4) m.toggleStep(0, s);
    for (int s = 4; s < 16; s += 8) m.toggleStep(1, s);
    for (int s = 2; s < 16; s += 4) m.toggleStep(2, s);
    m.select(1, 12);
    m.setVelocity(84);
    m.setProbability(75);
    m.setPitch(-2);
    m.setSwing(58);
    m.toggleMute(5);
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("step-sequencer");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("window.step_sequencer"), -1);
        auto* m = c.view.rootObject()->findChild<StepSequencerModel*>(
            QStringLiteral("stepSequencerModel"));
        if (m != nullptr) fill(*m);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
