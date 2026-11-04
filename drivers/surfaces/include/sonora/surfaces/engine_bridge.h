// SPDX-License-Identifier: GPL-3.0-or-later
// The engine side of the surface boundary (ADR-0040, ADR-0055): drains
// SurfaceEvents into the same commands and parameter changes the UI sends
// through api::EngineControl, and turns published state into SurfaceFeedback.
// It never touches the engine's internals and never runs on the audio thread;
// it belongs to whoever ticks the control client.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/api/engine_control.h"
#include "sonora/api/state_view.h"
#include "sonora/surfaces/surface_link.h"

namespace sonora::surfaces {

// The fader's travel 0..1 <-> linear gain 0..4. The same table as the Qt
// shell's fader_taper.cpp (+12 dB at the top, 0 dB at 78 %, -inf at the
// bottom); a test pins unity so the two cannot drift unseen.
float surface_gain_from_position(float position);
float surface_position_from_gain(float gain);

class EngineSurfaceBridge {
public:
    using PeakFn = std::function<float(std::uint32_t track)>;

    explicit EngineSurfaceBridge(api::EngineControl& engine) : engine_(engine) {}

    // Events -> commands. Fader touch brackets an automation gesture
    // (Gesture::Touch ... Release); a fader moved with no touch sense goes
    // as plain Move.
    void drain(SurfaceLink& link);

    // State -> feedback. Only what changed is pushed (a quantised meter step
    // counts), so a surface that stops draining costs drops in the counter,
    // not a stalled engine. `names` may be empty: strips then read "Trk NN".
    void publish(SurfaceLink& link, const api::StateFrame& frame, const PeakFn& peak,
                 const std::vector<std::string>& names);

private:
    struct Strip {
        bool arm = false, mute = false, solo = false, touched = false;
        float fader = -1.0f;
        int level = -1;
        bool named = false;
        std::string name;
    };
    void command(const char* id, int track);

    api::EngineControl& engine_;
    Strip strips_[kMaxStrips];
    int lit_[7] = {-1, -1, -1, -1, -1, -1, -1};
};

}  // namespace sonora::surfaces
