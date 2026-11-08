// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in manager over the plugin chooser: the reverb blocked by the
// operator (Unblock), a delay blocked whose bundle is gone (still listed,
// ADR-0065 §4), the probe's refusal with its words, the bridged compressor
// with its badge, and a favourite starred.
#include <string>

#include "mix_plugin_manager_scene.h"
#include "sonora/api/plugin_refusal.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-plugin-manager");
    s.window = QStringLiteral("mix");
    s.setup = [](ShotContext& c) {
        open_plugin_manager(
            c,
            [](std::vector<api::PluginInfo>& plugins) {
                const std::string blocked =
                    std::string(api::kBlockedRefusalPrefix) + ": it is blocked in the plug-in manager";
                for (api::PluginInfo& p : plugins) {
                    if (p.id == "urn:sonora:fake:reverb") {
                        p.usable = false;
                        p.refusal = blocked;
                    }
                }
                api::PluginInfo gone;
                gone.id = "urn:sonora:demo:tape-echo";
                gone.name = "Tape Echo";
                gone.usable = false;
                gone.refusal = blocked;
                plugins.push_back(gone);
            },
            [](PluginManagerModel& m) {
                // Starred once: the favourites file outlives the picture.
                const QString eq = QString::fromStdString(plugin_key(api::PluginFormat::Lv2, "urn:sonora:fake:eq"));
                for (const auto& r : m.all()) {
                    if (r.key == eq.toStdString() && !r.favourite) m.toggleFavourite(eq);
                }
            });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
