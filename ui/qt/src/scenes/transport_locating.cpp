// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window while the engine holds a locate back for its window
// and has played blocks as silence: "Locating..." in the state pill after
// 100 ms, and the red "Disk misses 3" beside it. Both come from the fake
// engine's scripted flag and counter, read through the same snapshot as the
// real engine's.
#include "control_adapter.h"
#include "sonora/api/fake_engine.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-locating");
    s.args = QStringLiteral("--keymap keymaps/protools.json");
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        auto* adapter = dynamic_cast<ControlAdapter*>(&c.model.engineAdapter());
        auto* fake = adapter ? dynamic_cast<api::testing::FakeEngine*>(&adapter->backend())
                             : nullptr;
        if (fake == nullptr) return;
        fake->add_playback_misses(3);
        fake->script_locating(true);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
