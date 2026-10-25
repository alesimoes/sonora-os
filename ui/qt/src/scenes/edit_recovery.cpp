// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window as a session opens with takes on disk it does not name:
// the engine's offer, one line per take saying where it would go, held
// until the operator places them or leaves them in Audio Files. The demo's
// engine has no disk, so the offer is the words the engine sends
// (orphan_offer.cpp), given to the shell as its RecoveryOffer would be.
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-recovery");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.showRecoveryOffer(
            QStringLiteral(
                "Audio 1_07.wav: take 7 on \"Kick\" at 12.000 s, 31.250 s long\n"
                "Audio 2_07.wav: take 7 on \"Snare\" at 12.000 s, 31.250 s long\n"
                "Audio 9_08.wav: take 8, punched on \"Bass DI\" from 20.000 s to 24.500 s\n"
                "Audio 11_09.wav: take 9, loop on \"Gtr L\": 4 passes from 8.000 s to 16.000 s on "
                "new playlists, placed by loop length\n"
                "Audio 17_03.wav: take 3, 6.200 s, into the audio pool only: its track is not in "
                "this session"),
            5);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
