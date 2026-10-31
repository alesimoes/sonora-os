// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora {

// What one pass of Touch, Latch or Write left on one parameter (ADR-0027
// §3), in the lane's unit: a span of `frames` frames played at `value`
// from `frame`, or with `frames` 0 the frame a released Touch parameter
// began its return to the lane from `value`.
struct PassWrite {
    model::Id track = 0;
    model::AutomationParam param = model::AutomationParam::Volume;
    model::Frames frame = 0;
    uint32_t frames = 0;
    float value = 0.0f;
};

// The label of a pass's one undo step.
inline constexpr const char* kAutomationWriteLabel = "Automation Write";

// The pass as one thinned EditAutomation per written lane, in the order the
// lanes were first written. Where a later write covers an earlier one (a
// loop's next turn), the later wins. Outside what was written each lane
// keeps what it played: its old points in the gaps between runs, its old
// value (or, with no lane, the static fader or pan) on each run's edges,
// and a release ramps back to it over `return_frames`, as the audio did.
// Writes on a track the document does not hold are dropped.
std::vector<model::EditAutomation> automation_pass_edits(const model::Snapshot& doc,
                                                         const std::vector<PassWrite>& writes,
                                                         model::Frames return_frames);

// Removes the points of a run that lie within `tolerance` of the line
// between the points kept either side of them. The first and last points,
// and both points of a step, are always kept.
void thin_automation(std::vector<model::AutomationPoint>& points, float tolerance);

}  // namespace sonora
