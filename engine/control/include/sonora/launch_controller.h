#pragma once

// The clip launcher's control half (ADR-0224 sections 2 to 4): what the UI
// and the key commands ask of the launcher, in process, on the control
// thread. It resolves a cell's clip to its region before the audio thread
// sees it, requests record-to-cell and bounce-to-arrangement, and keeps the
// performance log the audio thread forwards. Nothing here runs in
// Session::process(); the audio half is launch_voice_source.h.
//
// No wire command yet: no protocol version is reserved for the launcher, so
// the server reaches this interface in process only. The wire mapping is a
// later amendment with its own protocol number.

#include "sonora/launch/launch_grid.h"
#include "sonora/launch/launch_quantize.h"
#include "sonora/launch/launcher.h"
#include "sonora/launch_voice_source.h"

#include <cstdint>
#include <vector>

namespace sonora {

// Why a request was refused. Each is a held notice in the UI, not the status
// line: the operator pressed a cell and nothing will happen.
enum class LaunchRefusal : std::uint8_t {
    None,
    NoSession,           // no session is open
    OutOfGrid,           // track or scene outside the grid
    NoSuchRegion,        // the id names no region of the document
    RegionOnOtherTrack,  // a cell plays regions of its own track only
    EmptyRegion,         // a region of length 0 cannot loop
    QueueFull,           // the launcher's command queue overran; try again
    NotArmed,            // record-to-cell needs the track record-armed
    TakeOpen,            // a linear take is recording; the launcher waits
    CellRecording,       // the column already records into a cell
    BadRange,            // bounce range empty or reversed
    NothingToBounce,     // no clip played inside the range
};

struct LaunchReply {
    LaunchRefusal refusal = LaunchRefusal::None;
    bool ok() const { return refusal == LaunchRefusal::None; }
};

// A transition as it happened: Transition plus the absolute frame. The audio
// thread pushes one per transition into a ring (block_start + offset); a full
// ring is an overrun, counted, and the log has a hole the bounce reports.
struct LaunchEvent {
    launch::Transition::Kind kind = launch::Transition::Kind::Start;
    std::uint32_t track = 0;
    std::uint32_t scene = 0;
    launch::ClipId clip = launch::kNoClip;
    std::int64_t frame = 0;
};

inline LaunchEvent to_event(const launch::Transition& t, std::int64_t block_start) {
    return LaunchEvent{t.kind, t.track, t.scene, t.clip,
                       block_start + static_cast<std::int64_t>(t.offset)};
}

// Record into a cell (ADR-0224 section 3). The take starts on the launcher's
// next boundary on the track's input and ends `bars` bars later, or at the
// next trigger or stop on the column when bars is 0. Its region lands on the
// track's "Launcher" playlist, never the arrangement's, is assigned to the
// cell and plays from the boundary where the take ended.
struct RecordToCellRequest {
    std::uint32_t track = 0;
    std::uint32_t scene = 0;
    std::uint32_t bars = 0;
};

// Write what the launcher played in [from_frame, to_frame) into the
// arrangement (ADR-0224 section 4): one region per loop pass on each track's
// active playlist, overwriting what was there, as one undo step.
struct BounceToArrangementRequest {
    std::int64_t from_frame = 0;
    std::int64_t to_frame = 0;
};

struct BounceResult {
    LaunchRefusal refusal = LaunchRefusal::None;
    std::uint32_t regions_placed = 0;
    // Transitions lost to a full event ring inside the range: the arrangement
    // may miss a span, so the UI says so.
    std::uint64_t events_lost = 0;
    bool ok() const { return refusal == LaunchRefusal::None; }
};

// One region the bounce places: a loop pass of a played span.
struct PlacedPass {
    std::uint32_t track = 0;
    launch::ClipId clip = launch::kNoClip;
    std::int64_t position = 0;
    std::int64_t length = 0;
    std::int64_t source_offset = 0;
    // 0 for the first pass, k for the k-th repeat: the model's Region::loop
    // (ADR-0052), so the passes form one clip group like Loop Clip's.
    std::uint16_t loop = 0;
};

// Clip-to-region resolution of a played span [start, stop) seen through the
// bounce window [from, to): one pass per loop_frames counted from `start`,
// the last cut at `stop`. Passes are counted from where the clip really
// started, not from the window, so a span begun before `from` lands in phase:
// its first pass in the window is the tail of the pass that was sounding,
// with the source offset moved on to match. Control thread; appends to `out`.
inline void place_span(std::uint32_t track, const ResolvedClip& clip, std::int64_t start,
                       std::int64_t stop, std::int64_t from, std::int64_t to,
                       std::vector<PlacedPass>& out) {
    const std::int64_t end = stop < to ? stop : to;
    if (clip.loop_frames <= 0 || end <= start || end <= from)
        return;
    std::int64_t at = start;
    std::uint16_t loop = 0;
    if (at < from) {
        // Skip whole passes before the window without walking them.
        const std::int64_t skipped = (from - at) / clip.loop_frames;
        at += skipped * clip.loop_frames;
        loop = skipped >= UINT16_MAX ? UINT16_MAX : static_cast<std::uint16_t>(skipped);
    }
    for (; at < end; at += clip.loop_frames) {
        const std::int64_t pass_end = end - at < clip.loop_frames ? end : at + clip.loop_frames;
        const std::int64_t p = at < from ? from : at;
        if (pass_end > p)
            out.push_back(PlacedPass{track, clip.clip, p, pass_end - p,
                                     clip.source_offset + (p - at), loop});
        if (loop < UINT16_MAX)
            ++loop;
    }
}

// A cell as the control thread last heard of it: from the events, not by
// reading the audio thread's grid (ADR-0164 Consequences).
struct LaunchCellView {
    launch::ClipId clip = launch::kNoClip;
    bool playing = false;
    bool recording = false;
};

class LaunchController {
public:
    virtual ~LaunchController() = default;

    // Resolves `region` against the open document, publishes the resolved
    // clip to the audio side, then assigns it to the cell (in that order, so
    // the clip table is never behind the grid).
    virtual LaunchReply assign_region(std::uint32_t track, std::uint32_t scene,
                                      launch::ClipId region) = 0;
    virtual LaunchReply clear_cell(std::uint32_t track, std::uint32_t scene) = 0;
    virtual LaunchReply set_quantize(launch::Quantize q) = 0;
    virtual LaunchReply trigger_cell(std::uint32_t track, std::uint32_t scene) = 0;
    virtual LaunchReply trigger_scene(std::uint32_t scene) = 0;
    virtual LaunchReply stop_track(std::uint32_t track) = 0;
    virtual LaunchReply stop_all() = 0;

    virtual LaunchReply record_to_cell(const RecordToCellRequest& request) = 0;
    virtual BounceResult bounce_to_arrangement(const BounceToArrangementRequest& request) = 0;

    // Drains the audio thread's events into the performance log and the cell
    // views. Called by the control loop; also before every bounce.
    virtual void pump() = 0;
    virtual LaunchCellView cell(std::uint32_t track, std::uint32_t scene) const = 0;
    // Forgets the log and every cell: a session opened or closed.
    virtual void reset() = 0;
};

}  // namespace sonora
