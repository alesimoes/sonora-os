// SPDX-License-Identifier: GPL-3.0-or-later
//
// A version 3 file whose tempo map is outside the bounds: it opens
// modified, with one held notice per value the loader repaired.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-repairs");
    s.args = QStringLiteral("--open common/model/tests/fixtures/v3-tempo-out-of-bounds.sonora");
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
