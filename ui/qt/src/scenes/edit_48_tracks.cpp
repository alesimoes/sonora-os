// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window at 48 tracks, the density a UMC1820 session implies.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-48-tracks");
    s.args = QStringLiteral("--tracks 48");
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
