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
#include <QTimer>
#include <functional>
#include <string>
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

// The engine answers the demo session's Load a moment after the window is
// up, and an edit before that is refused ("still opening"). Runs `then`
// once it has answered, asking again every 100 ms within the capture's
// wait. Undo is the probe: it refuses with that reason while opening and
// changes nothing, since the demo session has no history to undo. The
// playlist pointer is taken again, as the Load left it dangling.
inline void when_open(ShotContext c, std::function<void(ShotContext&)> then, int tries = 0) {
    std::string why;
    const bool opening = !c.session.controller().document().undo(why) &&
                         why.find("still opening") != std::string::npos;
    if (!opening || tries >= 30) {
        // The Load replaced the snapshot the playlist pointer was taken from.
        const auto& tracks = c.session.controller().snapshot().edit->tracks;
        if (c.track >= 0 && c.track < static_cast<int>(tracks.size()))
            c.playlist = active_playlist(tracks[static_cast<size_t>(c.track)].get());
        then(c);
        return;
    }
    QTimer::singleShot(100, [c, then = std::move(then), tries]() mutable {
        when_open(c, std::move(then), tries + 1);
    });
}

struct ShotScene {
    QString name;
    QString window = QStringLiteral("edit");
    // sonora-ui arguments besides --demo and --scene, as the shell splits them.
    QString args;
    // A rolling transport pages the view to the playhead, away from what
    // the scene is meant to show.
    bool take = true;
    bool tempo_map = false;
    // The one scene that documents the software-rendering banner.
    bool software_banner = false;
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
