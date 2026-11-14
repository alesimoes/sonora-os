// SPDX-License-Identifier: GPL-3.0-or-later
//
// RenderInPlace's half of the render-scoped solo (ADR-0087 §4): asked of the
// hosts before the roll, waited for on the control thread as locate/settle
// wait for the disk, and lifted on every way out.
#include <chrono>
#include <thread>

#include "sonora/render_in_place.h"
#include "sonora/render_solo.h"

namespace sonora {

bool RenderInPlace::engage_solo(model::Id track, model::Id insert, std::string& error) {
    // Standing from the first post on: a host that took part of it must be
    // returned to the document's bypass even when the rest never confirmed.
    soloed_track_ = track;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kRenderSoloConfirmMs);
    while (!solo_->render_solo(track, insert)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            lift_solo();
            // A file that might hold the whole chain is worse than none.
            error = "the plug-in hosts did not confirm that the track's other inserts were "
                    "silenced, so the print could hold the whole chain; nothing was rendered";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void RenderInPlace::lift_solo() {
    if (soloed_track_ == 0 || solo_ == nullptr) return;
    solo_->render_unsolo(soloed_track_);
    soloed_track_ = 0;
}

}  // namespace sonora
