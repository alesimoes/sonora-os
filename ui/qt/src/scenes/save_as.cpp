// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > Save Copy In's dialog, as app.save_copy_in without a name opens
// it: the first free name offered, where it goes, and "All audio files"
// (daw.model.session_mgmt). Save As's is the same without the option.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-save-copy-in");
    s.take = false;
    s.apply = [](ShotContext& c) { c.model.invoke(QStringLiteral("app.save_copy_in"), -1); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
