// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "screenshot_scene.h"

namespace sonora::ui {

namespace {

// A function's static, not a namespace one: registrars in other files run
// during static initialisation, in an order nobody chooses.
std::vector<ShotScene>& scenes() {
    static std::vector<ShotScene> all;
    return all;
}

}  // namespace

ShotRegistrar::ShotRegistrar(ShotScene scene) { scenes().push_back(std::move(scene)); }

std::vector<ShotScene> registered_scenes() {
    std::vector<ShotScene> sorted = scenes();
    std::sort(sorted.begin(), sorted.end(),
              [](const ShotScene& a, const ShotScene& b) { return a.name < b.name; });
    return sorted;
}

const ShotScene* find_scene(const QString& name) {
    for (const ShotScene& s : scenes()) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

}  // namespace sonora::ui
