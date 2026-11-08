// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the plug-in manager's scenes share: the manager main() put on the
// view's context, and a catalog staged as the engine would send it, given
// to the chooser and the manager alike, after the engine's own catalog
// (sent a moment after the snapshot) has arrived and so cannot replace it.
#pragma once

#include <QQmlContext>
#include <QTimer>
#include <functional>
#include <vector>

#include "plugin_manager_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {

inline PluginManagerModel* scene_plugin_manager(ShotContext& c) {
    return qobject_cast<PluginManagerModel*>(
        c.view.rootContext()->contextProperty(QStringLiteral("pluginManager")).value<QObject*>());
}

// Opens the chooser on Lead Vox's fourth slot, then the manager over it on
// `plugins`, which `stage` builds from the engine's catalog.
inline void open_plugin_manager(ShotContext& c,
                                std::function<void(std::vector<api::PluginInfo>&)> stage,
                                std::function<void(PluginManagerModel&)> then = {}) {
    c.inserts.openBrowser(14, 3);
    PluginManagerModel* manager = scene_plugin_manager(c);
    if (manager == nullptr) {
        std::fprintf(stderr, "sonora-ui: scene: no plug-in manager on the view\n");
        return;
    }
    InsertModel* inserts = &c.inserts;
    QTimer::singleShot(1500, manager, [manager, inserts, stage = std::move(stage), then = std::move(then)] {
        api::PluginCatalog catalog;
        catalog.generation = 2;
        catalog.plugins = inserts->controller().plugins();
        stage(catalog.plugins);
        inserts->onCatalog(catalog);
        manager->onCatalog(catalog);
        manager->openManager();
        if (then) then(*manager);
    });
}

}  // namespace sonora::ui
