// SPDX-License-Identifier: GPL-3.0-or-later
//
// A take in progress, as Pro Tools draws one: Snare and Tom 1 armed (the
// demo's other arms taken off), the transport recording from bar 7 on the
// fake engine's input tones, and on each armed lane the new region growing
// under the red playhead in the record colour, its overview the track's
// input peaks. Hi-Hat, between them and not armed, shows its regions as
// they were. Zoomed in, so the take seconds into its run fills a good part
// of the lane.
#include <QTimer>
#include <array>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-recording");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        // The demo arms tracks 1 to 8; two stay armed here.
        for (const int t : std::array<int, 6>{0, 2, 4, 5, 6, 7}) {
            if (t < c.session.trackCount()) c.model.invoke(QStringLiteral("track.toggle_arm"), t);
        }
        const model::Frames bar = 2 * static_cast<model::Frames>(
                                          c.session.controller().snapshot().settings.sample_rate);
        EditController& edit = c.session.controller();
        edit.set_insertion(6 * bar);
        c.model.invokeWith(QStringLiteral("transport.locate"), -1, QString::number(6 * bar));
        for (int i = 0; i < 2; ++i) c.model.invoke(QStringLiteral("zoom.in"), -1);
        c.model.invoke(QStringLiteral("transport.record"), -1);
        // Where Record only makes the transport ready, Play starts the take.
        ShellModel* shell = &c.model;
        QTimer::singleShot(400, shell, [shell]() {
            if (!shell->recording()) shell->invoke(QStringLiteral("transport.play"), -1);
        });
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
