#pragma once

// How the launcher's transitions drive audio (ADR-0224 section 1). Every
// function here runs on the audio thread, after Launcher::advance() for the
// same block: no allocation, lock, log or throw. ClipTable is sized when the
// session is built; what it holds is resolved off the audio thread
// (launch_controller.h) and handed over through a ring the session drains
// before advance(), so the table is never older than the grid.

#include "sonora/launch/launch_grid.h"
#include "sonora/launch/launcher.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonora {

// A cell's clip as the audio thread plays it: a snapshot of the region the
// ClipId names (ADR-0164: a ClipId is the region's model id), taken when the
// clip was assigned. Edits to the region afterwards are a new assignment.
struct ResolvedClip {
    launch::ClipId clip = launch::kNoClip;
    // The source frame that plays at the clip's first frame (the region's
    // source_offset).
    std::int64_t source_offset = 0;
    // One pass of the loop: the region's length. A voice wraps back to
    // source_offset every loop_frames until it is stopped.
    std::int64_t loop_frames = 0;
    float gain_db = 0.0f;
};

// Audio thread. One voice per track column, as the launcher plays at most one
// cell per column. Both calls land on `offset` frames into the current block
// (Transition::offset), so the voice splits its block there.
class LaunchVoiceSource {
public:
    virtual ~LaunchVoiceSource() = default;
    // Starts `clip` on the track at `offset`, looping every clip.loop_frames.
    // A voice already sounding on the track has been stopped at the same
    // offset first (drive_voices keeps the launcher's stop-then-start order).
    virtual void start(std::uint32_t track, const ResolvedClip& clip, std::uint32_t offset) = 0;
    // Stops the track's voice at `offset`. No voice: nothing to do.
    virtual void stop(std::uint32_t track, std::uint32_t offset) = 0;
};

// One slot per cell, sized at construction and never resized.
class ClipTable {
public:
    ClipTable(std::uint32_t tracks, std::uint32_t scenes)
        : tracks_(tracks), scenes_(scenes),
          slots_(static_cast<std::size_t>(tracks) * scenes) {}

    std::uint32_t tracks() const { return tracks_; }
    std::uint32_t scenes() const { return scenes_; }

    // Audio thread (draining the ring). false: the cell is outside the grid.
    bool set(std::uint32_t track, std::uint32_t scene, const ResolvedClip& clip) {
        if (track >= tracks_ || scene >= scenes_)
            return false;
        slots_[index(track, scene)] = clip;
        return true;
    }

    // The cell's clip if it is the one the transition names and it can play;
    // nullptr otherwise. Matching the id guards against a resolution that has
    // not reached the table (its ring overran): that start is refused rather
    // than played with another clip's length.
    const ResolvedClip* find(std::uint32_t track, std::uint32_t scene,
                             launch::ClipId clip) const {
        if (track >= tracks_ || scene >= scenes_ || clip == launch::kNoClip)
            return nullptr;
        const ResolvedClip& slot = slots_[index(track, scene)];
        if (slot.clip != clip || slot.loop_frames <= 0)
            return nullptr;
        return &slot;
    }

private:
    std::size_t index(std::uint32_t track, std::uint32_t scene) const {
        return static_cast<std::size_t>(scene) * tracks_ + track;
    }

    std::uint32_t tracks_;
    std::uint32_t scenes_;
    std::vector<ResolvedClip> slots_;
};

// Counted, never logged: the audio thread reports them through the state
// block.
struct DriveCounts {
    // Starts refused because the table had no matching clip.
    std::uint64_t unresolved = 0;
    // Transitions the launcher could not list (TransitionList::dropped).
    std::uint64_t dropped = 0;
};

// Applies one block's transitions in list order, which is the launcher's:
// within a column a stop comes before the start on the same frame.
inline void drive_voices(const launch::TransitionList& list, const ClipTable& table,
                         LaunchVoiceSource& voices, DriveCounts& counts) {
    counts.dropped += list.dropped;
    for (std::uint32_t i = 0; i < list.count; ++i) {
        const launch::Transition& t = list[i];
        if (t.kind == launch::Transition::Kind::Stop) {
            voices.stop(t.track, t.offset);
            continue;
        }
        const ResolvedClip* clip = table.find(t.track, t.scene, t.clip);
        if (clip == nullptr) {
            ++counts.unresolved;
            continue;
        }
        voices.start(t.track, *clip, t.offset);
    }
}

}  // namespace sonora
