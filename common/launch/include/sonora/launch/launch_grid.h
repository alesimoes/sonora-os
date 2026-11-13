#pragma once

// The clip launcher grid (ADR-0164): tracks x scenes, sized once at creation.
// A cell refers to an existing region by id; the grid never owns audio.
// Storage is scene-major so a scene row is contiguous.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonora::launch {

using ClipId = std::uint64_t;
inline constexpr ClipId kNoClip = 0;
inline constexpr std::int32_t kNone = -1;

// Armed: a capture waits for its boundary. Recording: the cell is capturing;
// it gets its clip when the capture ends.
enum class CellState : std::uint8_t { Stopped, Queued, Playing, QueuedStop, Armed, Recording };

struct Cell {
    ClipId clip = kNoClip;
    CellState state = CellState::Stopped;
};

// A read-only view of one scene: one cell per track.
struct SceneRow {
    const Cell* cells = nullptr;
    std::uint32_t tracks = 0;
    const Cell& operator[](std::uint32_t track) const { return cells[track]; }
};

// The only allocation is in the constructor; nothing resizes afterwards, so
// the audio thread may read and write cells freely.
class LaunchGrid {
public:
    LaunchGrid(std::uint32_t tracks, std::uint32_t scenes)
        : tracks_(tracks), scenes_(scenes),
          cells_(static_cast<std::size_t>(tracks) * scenes) {}

    std::uint32_t tracks() const { return tracks_; }
    std::uint32_t scenes() const { return scenes_; }
    bool contains(std::uint32_t track, std::uint32_t scene) const {
        return track < tracks_ && scene < scenes_;
    }

    Cell& cell(std::uint32_t track, std::uint32_t scene) {
        return cells_[index(track, scene)];
    }
    const Cell& cell(std::uint32_t track, std::uint32_t scene) const {
        return cells_[index(track, scene)];
    }
    SceneRow scene_row(std::uint32_t scene) const {
        return SceneRow{cells_.data() + static_cast<std::size_t>(scene) * tracks_, tracks_};
    }

private:
    std::size_t index(std::uint32_t track, std::uint32_t scene) const {
        return static_cast<std::size_t>(scene) * tracks_ + track;
    }

    std::uint32_t tracks_;
    std::uint32_t scenes_;
    std::vector<Cell> cells_;
};

}  // namespace sonora::launch
