// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation.thin on the Edit window: a volume ride recorded as a dense run
// of breakpoints, a range of the lane selected, then thinned through the
// command (as the key map's ctrl+alt+t does). The picture is the lane after:
// the curve keeps its shape on far fewer breakpoints.
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void gestures(ShotContext c) {
    EditController& edit = c.session.controller();
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    c.model.invoke(QStringLiteral("zoom.vertical_in"), -1);
    const ViewState& v = edit.view();
    std::string ignored;
    edit.set_tool(Tool::Select);
    const double y = v.autolane_y(c.track, 0, 0.15f);
    edit.press(v.x_of(2 * bar), y, ignored);
    edit.move(v.x_of(14 * bar), y);
    edit.release(v.x_of(14 * bar), y, ignored);
    c.model.invoke(QStringLiteral("automation.thin"), -1);
    c.model.invoke(QStringLiteral("tool.select"), -1);
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
    for (int i = 0; i <= 160; ++i) {
        const double t = i / 160.0;
        op.points.push_back({static_cast<model::Frames>(t * 16 * bar),
                             static_cast<float>(-8.0 + 6.0 * std::sin(t * 6.2832 * 1.5))});
    }
    if (!edit.document().submit(op, why)) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: automation thin: %s\n", why.c_str());
        }
        return;
    }
    edit.toggle_autolanes(c.track, why);
    QTimer::singleShot(300, [c]() { gestures(c); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-automation-thin");
    s.take = false;
    s.apply = [](ShotContext& c) { build(c, 0); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
