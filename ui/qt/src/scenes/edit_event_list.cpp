// SPDX-License-Identifier: GPL-3.0-or-later
//
// The automation Event List over the Edit window: a volume ride of a few
// breakpoints on the snare, its lane shown, the window listing the points
// with their time and value, and a Snapshot taken at the insertion point
// (the last row, written from the fader as it stands).
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void gestures(ShotContext c) {
    EditController& edit = c.session.controller();
    c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    edit.select_track(c.track);
    edit.set_insertion(static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 26));
    c.model.invoke(QStringLiteral("automation.snapshot"), -1);
    c.model.invoke(QStringLiteral("window.event_list"), -1);
}

void build(ShotContext c, int tries) {
    EditController& edit = c.session.controller();
    std::string why;
    const model::Id id = edit.snapshot().edit->tracks[static_cast<std::size_t>(c.track)]->id;
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    model::EditAutomation op;
    op.track = id;
    op.param = model::AutomationParam::Volume;
    op.from = 0;
    op.to = model::kMaxInteger;
    const float ride[] = {-12.0f, -6.0f, -6.0f, -2.5f, -9.0f, -9.0f, -4.0f};
    const model::Frames at[] = {0, 2 * bar, 2 * bar, 5 * bar, 8 * bar, 9 * bar, 11 * bar};
    for (int i = 0; i < 7; ++i) op.points.push_back({at[i], ride[i]});
    if (!edit.document().submit(op, why)) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: event list: %s\n", why.c_str());
        }
        return;
    }
    edit.toggle_autolanes(c.track, why);
    QTimer::singleShot(300, [c]() { gestures(c); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-event-list");
    s.take = false;
    s.apply = [](ShotContext& c) { build(c, 0); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
