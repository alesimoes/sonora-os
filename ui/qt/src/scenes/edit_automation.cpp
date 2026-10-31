// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window with the snare's automation lanes disclosed: a volume ride
// and a pan sweep, a range selected on the volume lane and a breakpoint
// picked, all made through the controller's own gestures so the picture is
// what an engineer's hand leaves.
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

// The gestures, once the lanes are in the document.
void gestures(ShotContext c) {
    EditController& edit = c.session.controller();
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    c.model.invoke(QStringLiteral("zoom.vertical_in"), -1);   // the lanes grow with it
    const ViewState& v = edit.view();
    std::string ignored;
    edit.set_tool(Tool::Select);
    const double y = v.autolane_y(c.track, 0, 0.15f);
    edit.press(v.x_of(4 * bar), y, ignored);
    edit.move(v.x_of(9 * bar), y);
    edit.release(v.x_of(9 * bar), y, ignored);
    edit.set_tool(Tool::Smart);
    const double py =
        v.autolane_y(c.track, 0, lane_position(model::AutomationParam::Volume, -2.0f));
    edit.press(v.x_of(5 * bar), py, ignored);
    edit.release(v.x_of(5 * bar), py, ignored);
    c.model.invoke(QStringLiteral("tool.smart"), -1);   // re-publishes what the gestures changed
}

// The demo session may still be opening on the engine, which refuses edits
// until it has answered: the first one is asked again until it goes.
void build(ShotContext c, int tries) {
    EditController& edit = c.session.controller();
    std::string why;
    const model::Id id = edit.snapshot().edit->tracks[static_cast<std::size_t>(c.track)]->id;
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    if (!put(edit, id, model::AutomationParam::Volume,
             {{0, -8.0f}, {4 * bar, -8.0f}, {5 * bar, -2.0f}, {8 * bar, -2.0f},
              {9 * bar, -14.0f}, {12 * bar, -14.0f}, {13 * bar, -4.0f}, {20 * bar, -4.0f}},
             why)) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: automation: %s\n", why.c_str());
        }
        return;
    }
    put(edit, id, model::AutomationParam::Pan,
        {{0, -0.6f}, {6 * bar, 0.6f}, {12 * bar, -0.6f}, {18 * bar, 0.6f}}, why);
    edit.toggle_autolanes(c.track, why);
    c.model.invoke(QStringLiteral("tool.smart"), -1);
    QTimer::singleShot(300, [c]() { gestures(c); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-automation");
    s.take = false;
    s.apply = [](ShotContext& c) { build(c, 0); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
