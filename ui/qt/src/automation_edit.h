// SPDX-License-Identifier: GPL-3.0-or-later
//
// Editing an automation lane (ADR-0027 §1): the pure part. Every gesture
// (a breakpoint added, moved or deleted, a pencil stroke, cut, copy, paste,
// clear) is built here as one model::EditAutomation, which replaces a lane's
// points in a frame range, so each is one undo step and the model's own
// checks decide what is legal. Nothing here is Qt, and nothing is drawn.
//
// The workflow is Pro Tools' (ADR-0004; Avid's Pro Tools Reference Guide,
// "Automation" chapter): breakpoints with the Grabber, free drawing with the
// Pencil, and the Edit commands on a range of a lane.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::ui {

// The lanes a track discloses, top to bottom: what the model automates.
inline constexpr int kLaneCount = 2;
model::AutomationParam lane_param(int lane);
const char* lane_name(model::AutomationParam p);

// Where a value sits in a lane, 0 at the bottom to 1 at the top. Volume
// follows the fader's taper, so the curve is drawn as the fader moves; pan
// runs left at the bottom to right at the top.
float lane_position(model::AutomationParam p, float value);
float lane_value(model::AutomationParam p, float position);
float clamp_lane_value(model::AutomationParam p, float value);
// "-6.0 dB", "<30": the value as the strip says it.
std::string lane_value_text(model::AutomationParam p, float value);

// The lane's value at `at`, as the engine plays it: linear between points,
// held outside them, the second of a step's two points at the step.
// `fallback` (the static fader or pan) when there is no lane.
float lane_value_at(const model::AutomationLane* lane, model::Frames at, float fallback);

// The index of the point nearest `at` and `value` within `slop_frames` and
// `slop_position` (a lane position), -1 when none is that near.
int point_near(const model::AutomationLane* lane, model::AutomationParam p, model::Frames at,
               float value, double slop_frames, double slop_position);

// A breakpoint at `at`, replacing what was there.
model::EditAutomation add_point_op(model::Id track, model::AutomationParam p, model::Frames at,
                                   float value);

// Where a point dragged to `at`, `value` is left: `value` kept in the lane's
// range, `at` kept between the point's neighbours (one of a step's two
// points keeps its frame). `index` must be a point of `lane`.
model::AutomationPoint moved_point(const model::AutomationLane& lane, std::size_t index,
                                   model::Frames at, float value);

// Point `index` moved to `at`, `value`. A point cannot pass its neighbours (a
// move that would reorder the lane stops against them), and one of a step's
// two points keeps its frame. False when `index` is not a point.
bool move_point_op(model::Id track, const model::AutomationLane& lane, std::size_t index,
                   model::Frames at, float value, model::EditAutomation& out);

// Point `index` removed; the other point of its step, if any, stays.
bool delete_point_op(model::Id track, const model::AutomationLane& lane, std::size_t index,
                     model::EditAutomation& out);

// A pencil stroke: `samples`, in the order they were drawn (a stroke may go
// back on itself; the latest sample at a frame wins), thinned to the fewest
// breakpoints that keep the curve within the lane's tolerance, replacing the
// lane from the first sample to the last. False for no samples.
bool stroke_op(model::Id track, model::AutomationParam p,
               const std::vector<model::AutomationPoint>& samples,
               model::EditAutomation& out);

// `points` (sorted) with every point the line between its neighbours already
// passes within `tolerance` (lane units) removed; the first and last stay.
std::vector<model::AutomationPoint> thin_points(const std::vector<model::AutomationPoint>& points,
                                                float tolerance);
float thin_tolerance(model::AutomationParam p);

// What Copy and Cut hold: the lane's part of [a, b), positions from the
// range's start, with a point at each edge carrying the lane's value there,
// so the pasted piece has the shape it was copied with.
struct AutomationClip {
    model::AutomationParam param = model::AutomationParam::Volume;
    model::Frames length = 0;
    std::vector<model::AutomationPoint> points;
    bool empty() const { return points.empty(); }
};

// False (and `out` untouched) when `lane` is null: nothing to copy.
bool copy_lane_range(const model::AutomationLane* lane, model::Frames a, model::Frames b,
                     AutomationClip& out);

// [a, b) cleared: the lane keeps its shape outside, with a point at each
// edge holding the value there, and runs straight between them. A range
// that leaves no point outside it removes the lane. False when the lane has
// no point in the range or already holds only what a clear leaves.
bool clear_range_op(model::Id track, const model::AutomationLane& lane, model::Frames a,
                    model::Frames b, model::EditAutomation& out);

// The breakpoints inside [a, b) thinned the way a Pencil stroke is, steps
// kept. False when nothing would go (fewer than three points, or all needed):
// no edit, no undo step.
bool thin_range_op(model::Id track, const model::AutomationLane& lane, model::Frames a,
                   model::Frames b, model::EditAutomation& out);

// `clip` laid over [at, at + clip.length).
bool paste_op(model::Id track, const AutomationClip& clip, model::Frames at,
              model::EditAutomation& out);

// Automation Follows Edit (Pro Tools' Options menu; ADR-0027 §7): what a
// region edit on `track` adds, each lane's EditAutomation, to go in one
// Compound with it. Unlike Paste, both keep the lane exactly outside the
// spans: each span's ends become steps, the value before on one side and
// after on the other. A flat lane, and a move to where the region is, add
// nothing.
//
// The region moved from [from, to) to `at`: its part of each lane goes
// with it, and the place it left runs straight across, as a clear leaves it.
std::vector<model::EditAutomation> follow_move_ops(const model::TrackEdit& track,
                                                   model::Frames from, model::Frames to,
                                                   model::Frames at);
// [from, to) cleared on every lane with a point in [from, to]: it runs
// straight from the value before `from` to the value at `to`.
std::vector<model::EditAutomation> follow_clear_ops(const model::TrackEdit& track,
                                                    model::Frames from, model::Frames to);

}  // namespace sonora::ui
