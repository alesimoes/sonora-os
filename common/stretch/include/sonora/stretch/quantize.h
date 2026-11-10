// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/model/musical_map.h"
#include "sonora/stretch/warp_map.h"

namespace sonora::stretch {

struct QuantizeRequest {
    std::vector<std::int64_t> onsets;  // frames from the region's first frame, any order
    std::int64_t region_start = 0;     // session frame of the region's first frame
    std::int64_t region_length = 0;
    double grid_beats = 0.25;          // in beats of the meter in force (0.25 = 16th at 4/4)
    std::uint32_t strength = 100;      // percent, 1..100
};

// Moves each onset towards its nearest grid line by `strength` percent. The
// grid is counted from the tempo entry in force. Markers never cross: an
// onset that would land on or before the previous one's target is left out.
// False, with the reason, for: an empty map or bad grid, strength 0 or over
// 100, an empty region, or no usable onset. `out` is then untouched.
bool quantize_plan(const QuantizeRequest& req, const model::MusicalMap& map, WarpMap& out,
                   std::string& why);

}  // namespace sonora::stretch
