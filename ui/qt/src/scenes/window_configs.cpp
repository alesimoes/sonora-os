// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window Configurations: configuration 1 was stored with the Mix window in
// front, the Transport and Memory Locations windows open and moved from
// their usual places; the screen then went back to Edit with everything
// closed, and configuration 1 was recalled, as a click on its Recall button
// does. The list stays open beside them.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("window-configs");
    s.take = false;
    s.tempo_map = true;
    s.apply = [](ShotContext& c) {
        // No file: a picture must not write the machine's own slots.
        c.session.setLayoutsFile(QString());
        c.session.setWindow(QStringLiteral("mix"));
        c.transport.setOpen(true);
        c.session.setMemoryLocationsOpen(true);
        c.session.windowPlaced(QStringLiteral("transport"), 520, 110);
        c.session.windowPlaced(QStringLiteral("memory_locations"), 90, 330);
        std::string why;
        c.session.storeWindowConfig(1, why);
        c.session.setWindow(QStringLiteral("edit"));
        c.transport.setOpen(false);
        c.session.setMemoryLocationsOpen(false);
        c.session.recallWindowConfig(1, why);
        c.session.setWindowConfigsOpen(true);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
