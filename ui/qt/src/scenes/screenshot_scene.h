// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screenshot scenes: one file each under ui/qt/src/scenes/, registered
// by a static ShotRegistrar, so a UI change that adds a scene touches no
// shared file and two of them never conflict. `sonora-ui --list-scenes`
// prints them; tools/build-ui-entrypoint.sh shoots every one it lists, with
// `--demo --scene NAME` and the scene's own arguments.
//
// The name is the picture's: "edit" is the --screenshot file itself, "edit-X"
// is X after its stem, and anything else follows the stem with "-edit"
// dropped ("mix-browser" gives sonora-ui-mix-browser.png).
#pragma once

#include <QQuickView>
#include <QString>
#include <functional>
#include <vector>

#include "insert_model.h"
#include "model_access.h"
#include "send_model.h"
#include "session_model.h"
#include "shell_model.h"
#include "transport_model.h"

namespace sonora::ui {

// What a scene's hooks reach, 200 ms after the window is up on the demo.
struct ShotContext {
    SessionModel& session;
    ShellModel& model;
    InsertModel& inserts;
    SendModel& sends;
    TransportModel& transport;
    QQuickView& view;
    int track = 0;                              // the snare's, selected
    const model::Playlist* playlist = nullptr;  // its active playlist
};

struct ShotScene {
    QString name;
    QString window = QStringLiteral("edit");
    // sonora-ui arguments besides --demo and --scene, as the shell splits them.
    QString args;
    // A rolling transport pages the view to the playhead, away from what
    // the scene is meant to show.
    bool take = true;
    bool tempo_map = false;
    // Before the whole session is fitted in view and the snare selected.
    std::function<void(ShotContext&)> setup;
    // After that.
    std::function<void(ShotContext&)> apply;
};

// Sorted by name.
std::vector<ShotScene> registered_scenes();
const ShotScene* find_scene(const QString& name);

struct ShotRegistrar {
    explicit ShotRegistrar(ShotScene scene);
};

}  // namespace sonora::ui
