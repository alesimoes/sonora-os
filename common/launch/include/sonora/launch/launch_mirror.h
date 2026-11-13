#pragma once

// The control thread's copy of the launch grid (ADR-0164). The audio thread
// owns the launcher's grid; the control side rebuilds what it needs from the
// clip assignments it sends and the transitions the audio thread forwards
// (with their absolute launch-clock frame) through a ring. Off the audio
// thread: bounce and the UI read this, never Launcher::grid().
//
// Queued and Armed are not forwarded (they live between a command and its
// boundary); the mirror knows clips, Playing and Recording.

#include "sonora/launch/launch_grid.h"
#include "sonora/launch/launcher.h"

#include <cstdint>

namespace sonora::launch {

class LaunchMirror {
public:
    LaunchMirror(std::uint32_t tracks, std::uint32_t scenes) : grid_(tracks, scenes) {}

    // Mirrors a Launcher::assign_clip the control thread sent.
    void assign(std::uint32_t track, std::uint32_t scene, ClipId clip);
    // Applies one forwarded transition; out-of-grid indices are ignored.
    void apply(const Transition& tr);

    const LaunchGrid& grid() const { return grid_; }

private:
    LaunchGrid grid_;
};

}  // namespace sonora::launch
