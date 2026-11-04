// SPDX-License-Identifier: GPL-3.0-or-later
//
// Comping, the Pro Tools way (ADR-0031, ADR-0049): the lead vocal's two other
// takes shown as lanes under it, two ranges copied to the main playlist (the
// comp names the take each piece came from, the lane tints what is kept) and a
// third dragged out on take 2 and waiting for To Main.
#include "comping_scene.h"

namespace sonora::ui {
namespace {

using namespace comping_scene;

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-comping");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            if (!setup(c)) return;
            swipe(c, 0, 4, 8);
            copy_to_main(c);
            swipe(c, 1, 9, 12);
            copy_to_main(c);
            swipe(c, 0, 13, 16);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
