// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix groups (ADR-0041): four drum strips grouped as "Drums" and brought down
// together (one fader move, every member's cap), a suspended "Room" pair that
// keeps its own faders, and a muted "Guitars" group whose mute followed.
#include <QString>
#include <cstdio>
#include <string>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

bool step(bool ok, const std::string& why, const char* what) {
    if (!ok) std::fprintf(stderr, "sonora-ui: groups scene: %s: %s\n", what, why.c_str());
    return ok;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-groups");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c0) {
        when_open(c0, [](ShotContext& c) {
            EditController& e = c.session.controller();
            constexpr std::uint8_t kMix = model::kGroupVolume | model::kGroupMute |
                                          model::kGroupSolo | model::kGroupArm;
            std::string why;
            if (c.session.trackCount() < 8) return;
            if (!step(e.group_tracks(0, 4, "Drums", kMix, why), why, "Drums")) return;
            if (!step(e.group_tracks(4, 2, "Room", kMix, why), why, "Room")) return;
            if (!step(e.group_tracks(6, 2, "Guitars", kMix, why), why, "Guitars")) return;
            // The Room pair is suspended before its fader moves, so it stays
            // apart from the group's.
            if (!step(e.toggle_group_suspend(4, why), why, "Room suspend")) return;
            if (!step(e.set_gain(5, -7.5f, why), why, "Room fader")) return;
            if (!step(e.set_gain(1, -5.0f, why), why, "Drums fader")) return;
            if (!step(e.toggle_mute(6, why), why, "Guitars mute")) return;
            c.session.selectTrack(1);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
