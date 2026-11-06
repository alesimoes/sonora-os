// SPDX-License-Identifier: GPL-3.0-or-later
//
// Spot mode (F3): a region selected and the Spot dialog open on it, offering
// its start in the main counter's format. The time typed places it exactly.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-spot");
    s.take = false;
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        if (c.playlist == nullptr || c.playlist->regions.size() <= 1) return;
        const auto second = c.playlist->regions[1].id;
        c.model.invoke(QStringLiteral("edit.mode.spot"), -1);
        c.session.controller().select_region(second);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        c.model.invoke(QStringLiteral("edit.spot"), -1);
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
