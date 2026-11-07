// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in manager when the engine's scan found nothing: it says so and
// what to do, rather than drawing an empty list that reads as "loading".
#include "mix_plugin_manager_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-plugin-manager-empty");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) {
        open_plugin_manager(c, [](std::vector<api::PluginInfo>& plugins) { plugins.clear(); });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
