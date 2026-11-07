// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with the plugin chooser open on Lead Vox's fourth slot,
// the EQ starred as a favourite (the star, the Favourites filter and the
// Manage button are the plug-in manager's, ADR-0065).
#include "mix_plugin_manager_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-browser");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) {
        c.inserts.openBrowser(14, 3);
        PluginManagerModel* manager = scene_plugin_manager(c);
        if (manager == nullptr) return;
        // After the engine's catalog, which the star is looked up in.
        QTimer::singleShot(1500, manager, [manager] {
            const std::string eq = plugin_key(api::PluginFormat::Lv2, "urn:sonora:fake:eq");
            for (const auto& r : manager->all()) {
                if (r.key == eq && !r.favourite) manager->toggleFavourite(QString::fromStdString(eq));
            }
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
