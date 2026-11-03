// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Volume Trim lane (ADR-0027 §8 slice 5) under the snare, with the
// snare in Trim: the Volume lane holds the mix's ride, the Volume Trim lane
// a pass of trim over it in its own colour, about 0 dB at unity's height,
// and a range selected across the trim lanes of the snare and the track
// under it with the Selector, as an Edit command on them would take it.
#include <QTimer>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

bool put(EditController& edit, model::Id track, model::AutomationParam p,
         std::vector<model::AutomationPoint> pts, std::string& why) {
    model::EditAutomation op;
    op.track = track;
    op.param = p;
    op.from = 0;
    op.to = model::kMaxInteger;
    op.points = std::move(pts);
    return edit.document().submit(op, why);
}

void gestures(ShotContext c) {
    EditController& edit = c.session.controller();
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    const ViewState& v = edit.view();
    std::string ignored;
    edit.set_tool(Tool::Select);
    const double y0 = v.autolane_y(c.track, 2, 0.5f);
    const double y1 = v.autolane_y(c.track + 1, 2, 0.5f);
    edit.press(v.x_of(6 * bar), y0, ignored);
    edit.move(v.x_of(11 * bar), y1);
    edit.release(v.x_of(11 * bar), y1, ignored);
    c.model.invoke(QStringLiteral("tool.smart"), -1);   // re-publishes what the gestures changed
}

void build(ShotContext c, int tries) {
    EditController& edit = c.session.controller();
    std::string why;
    const auto& tracks = edit.snapshot().edit->tracks;
    if (c.track + 1 >= static_cast<int>(tracks.size())) {
        std::fprintf(stderr, "sonora-ui: trim lane scene: no track under the snare\n");
        return;
    }
    const model::Id id = tracks[static_cast<std::size_t>(c.track)]->id;
    const model::Id below = tracks[static_cast<std::size_t>(c.track) + 1]->id;
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    if (!put(edit, id, model::AutomationParam::Volume,
             {{0, -6.0f}, {8 * bar, -6.0f}, {9 * bar, -10.0f}, {20 * bar, -10.0f}}, why)) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: automation: %s\n", why.c_str());
        }
        return;
    }
    put(edit, id, model::AutomationParam::VolumeTrim,
        {{0, 0.0f}, {4 * bar, 0.0f}, {6 * bar, 2.5f}, {10 * bar, 2.5f}, {12 * bar, -4.0f},
         {16 * bar, -4.0f}, {17 * bar, 0.0f}},
        why);
    put(edit, below, model::AutomationParam::VolumeTrim,
        {{0, -2.0f}, {9 * bar, -2.0f}, {10 * bar, 1.5f}}, why);
    if (!edit.set_automation_mode(c.track, model::AutomationMode::Trim, why))
        std::fprintf(stderr, "sonora-ui: automation mode: %s\n", why.c_str());
    edit.toggle_autolanes(c.track, why);
    edit.toggle_autolanes(c.track + 1, why);
    c.model.invoke(QStringLiteral("tool.smart"), -1);
    QTimer::singleShot(300, [c]() { gestures(c); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-automation-trim");
    s.take = false;
    s.apply = [](ShotContext& c) { build(c, 0); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
