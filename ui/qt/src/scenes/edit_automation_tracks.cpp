// SPDX-License-Identifier: GPL-3.0-or-later
//
// A range of the volume lane dragged with the Selector across three tracks,
// the middle one's lanes hidden, then cleared through the command (Delete in
// the key maps). The picture: the range drawn on both shown volume lanes,
// both cleared in one undo step, and the held notice naming the hidden track
// the Clear left alone.
#include <QTimer>
#include <algorithm>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

bool put(EditController& edit, int index, std::vector<model::AutomationPoint> pts,
         std::string& why) {
    model::EditAutomation op;
    op.track = edit.snapshot().edit->tracks[static_cast<std::size_t>(index)]->id;
    op.param = model::AutomationParam::Volume;
    op.from = 0;
    op.to = model::kMaxInteger;
    op.points = std::move(pts);
    return edit.document().submit(op, why);
}

int last_of(const ShotContext& c) {
    const int n = static_cast<int>(c.session.controller().snapshot().edit->tracks.size());
    return std::min(c.track + 2, n - 1);
}

void gestures(ShotContext c) {
    EditController& edit = c.session.controller();
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    const ViewState& v = edit.view();
    std::string ignored;
    edit.set_tool(Tool::Select);
    const int last = last_of(c);
    edit.press(v.x_of(4 * bar), v.autolane_y(c.track, 0, 0.5f), ignored);
    edit.move(v.x_of(11 * bar), v.autolane_y(last, 0, 0.5f));
    edit.release(v.x_of(11 * bar), v.autolane_y(last, 0, 0.5f), ignored);
    c.model.invoke(QStringLiteral("edit.clear"), -1);
    c.model.invoke(QStringLiteral("tool.select"), -1);
}

void build(ShotContext c, int tries) {
    EditController& edit = c.session.controller();
    std::string why;
    const auto bar = static_cast<model::Frames>(edit.snapshot().settings.sample_rate * 2);
    const std::vector<model::AutomationPoint> ride{
        {0, -8.0f},        {3 * bar, -8.0f},   {5 * bar, -2.0f},  {7 * bar, -12.0f},
        {9 * bar, -3.0f},  {12 * bar, -10.0f}, {15 * bar, -4.0f}, {20 * bar, -4.0f}};
    if (!put(edit, c.track, ride, why)) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: automation tracks: %s\n", why.c_str());
        }
        return;
    }
    const int last = last_of(c);
    for (int i = c.track + 1; i <= last; ++i) put(edit, i, ride, why);
    edit.toggle_autolanes(c.track, why);
    if (last > c.track + 1) edit.toggle_autolanes(last, why);
    QTimer::singleShot(300, [c]() { gestures(c); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-automation-tracks");
    s.take = false;
    s.apply = [](ShotContext& c) { when_open(c, [](ShotContext& o) { build(o, 0); }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
