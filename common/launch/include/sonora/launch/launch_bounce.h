#pragma once

// Bounce of a launcher cell or scene to the arrangement (ADR-0164). Control
// thread only: the result allocates. The launcher knows clips by id, not
// their audio, so a bounce is a list of placements: put clip C on track T
// from frame `start` for `length` frames, looping the clip to fill it. The
// arrangement writer turns each placement into a region edit (one undo step
// for the whole bounce); empty cells produce nothing.

#include "sonora/launch/launch_grid.h"

#include <cstdint>
#include <vector>

namespace sonora::launch {

struct Placement {
    std::uint32_t track = 0;
    std::uint32_t scene = 0;
    ClipId clip = kNoClip;
    // Transport frames on the arrangement timeline.
    std::int64_t start = 0;
    std::int64_t length = 0;
};

// One placement per filled cell of the scene, in track order. A length of
// zero or less, or a scene out of the grid, yields nothing.
std::vector<Placement> bounce_scene(const LaunchGrid& grid, std::uint32_t scene,
                                    std::int64_t start, std::int64_t length);

// The cell's placement, or nothing for an empty or out-of-grid cell.
std::vector<Placement> bounce_cell(const LaunchGrid& grid, std::uint32_t track,
                                   std::uint32_t scene, std::int64_t start, std::int64_t length);

}  // namespace sonora::launch
