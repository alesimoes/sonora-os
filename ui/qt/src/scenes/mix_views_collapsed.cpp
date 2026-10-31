// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with I/O, inserts and sends collapsed: their height is the
// fader's (ui-design.md, "Mixer strip anatomy"). A plugin in a collapsed
// bank shows as "+N".
#include <QQuickItem>

#include "mix_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-views-collapsed");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        auto* mixer = c.view.rootObject()->property("mixer").value<MixModel*>();
        if (mixer == nullptr) return;
        mixer->setShowIO(false);
        mixer->setShowInsertsAE(false);
        mixer->setShowSends(false);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
