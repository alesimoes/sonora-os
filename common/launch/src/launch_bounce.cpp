#include "sonora/launch/launch_bounce.h"

namespace sonora::launch {

std::vector<Placement> bounce_cell(const LaunchGrid& grid, std::uint32_t track,
                                   std::uint32_t scene, std::int64_t start, std::int64_t length) {
    std::vector<Placement> out;
    if (length <= 0 || !grid.contains(track, scene)) return out;
    const ClipId clip = grid.cell(track, scene).clip;
    if (clip == kNoClip) return out;
    out.push_back(Placement{track, scene, clip, start, length});
    return out;
}

std::vector<Placement> bounce_scene(const LaunchGrid& grid, std::uint32_t scene,
                                    std::int64_t start, std::int64_t length) {
    std::vector<Placement> out;
    if (length <= 0 || scene >= grid.scenes()) return out;
    for (std::uint32_t t = 0; t < grid.tracks(); ++t) {
        const ClipId clip = grid.cell(t, scene).clip;
        if (clip != kNoClip) out.push_back(Placement{t, scene, clip, start, length});
    }
    return out;
}

}  // namespace sonora::launch
