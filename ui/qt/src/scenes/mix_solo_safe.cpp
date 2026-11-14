// SPDX-License-Identifier: GPL-3.0-or-later
//
// A vocal soloed over a session whose room mic and talkback are solo safe:
// those two stay audible and say so, with recessed solo buttons and a
// padlock, and the rest are outlined as the solo silences them.
#include <cstdio>
#include <string>

#include "screenshot_scene.h"
#include "sonora/model/operations.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-solo-safe");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        EditController& edit = c.session.controller();
        std::string why;
        for (const int t : {7, 17}) {
            if (t < c.session.trackCount() && !edit.toggle_solo_safe(t, why))
                std::fprintf(stderr, "sonora-ui: solo safe: %s\n", why.c_str());
        }
        if (14 < c.session.trackCount()) c.model.invoke(QStringLiteral("track.toggle_solo"), 14);
    }); };
    return s;
}()};

// A stereo overhead pair first in the strips, leaning left: its pan is a
// balance with L and R end marks.
const ShotRegistrar stereo_registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("pan-stereo");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        EditController& edit = c.session.controller();
        model::AddTrack add;
        add.track = edit.document().reserve_id();
        add.playlist = edit.document().reserve_id();
        add.name = "Overheads";
        add.playlist_name = "Overheads.01";
        add.channels = 2;
        add.mix.input = {16, 17};
        // The demo fills the engine's inputs: the last two strips make room.
        const auto& tracks = edit.snapshot().edit->tracks;
        std::vector<model::Id> order{add.track};
        for (std::size_t i = 0; i + 2 < tracks.size(); ++i) order.push_back(tracks[i]->id);
        std::string why;
        model::Compound both{{model::RemoveTrack{tracks.back()->id},
                              model::RemoveTrack{tracks[tracks.size() - 2]->id}, add, model::ReorderTracks{order}},
                             "New Track"};
        if (!edit.document().submit(both, why) || !edit.set_pan(0, -0.4f, why))
            std::fprintf(stderr, "sonora-ui: pan-stereo: %s\n", why.c_str());
    }); };
    return s;
}()};

// The same strip order with a mono track leaning right: the pan as it was.
const ShotRegistrar mono_registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("pan-mono");
    s.window = QStringLiteral("mix");
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        std::string why;
        if (!c.session.controller().set_pan(0, 0.4f, why))
            std::fprintf(stderr, "sonora-ui: pan-mono: %s\n", why.c_str());
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
