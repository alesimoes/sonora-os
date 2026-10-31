// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with every view expanded: inserts A to J and sends F to J,
// the floor of the fader share being 35 % (ui-design.md, "Mixer strip
// anatomy").
#include <QQuickItem>

#include "mix_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-views-expanded");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        auto* mixer = c.view.rootObject()->property("mixer").value<MixModel*>();
        if (mixer == nullptr) return;
        mixer->setShowInsertsFJ(true);
        c.sends.setBank(5);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
