// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonora/automation_write.h"
#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora {

// What one pass of Touch, Latch or Write left on one parameter (ADR-0027
// §3), in the lane's unit: a span of `frames` frames played at `value`
// from `frame`, or with `frames` 0 the frame a released Touch parameter
// began its return to the lane from `value`. With `line` set the span is
// a line from `value` to `to` at `frame + frames`, what PassWrites leaves
// of thinned-out writes.
struct PassWrite {
    model::Id track = 0;
    model::AutomationParam param = model::AutomationParam::Volume;
    model::Frames frame = 0;
    uint32_t frames = 0;
    float value = 0.0f;
    float to = 0.0f;
    bool line = false;
};

// A pass's writes as they are drained. A write that continues its lane's
// run is merged into it while the run stays within half the lane's
// tolerance of a line (the stop thins again, so the two stay within 1.5
// times it), which keeps a held or smoothly ridden lane at a few entries
// however long the roll.
class PassWrites {
public:
    void add(const PassWrite& w);
    void clear();
    void reserve(std::size_t n) { writes_.reserve(n); }
    const std::vector<PassWrite>& writes() const { return writes_; }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    struct Lane {
        model::Id track = 0;
        model::AutomationParam param = model::AutomationParam::Volume;
        // The run's last kept span, which merged writes extend, and the
        // newest write after it.
        std::size_t anchor = kNone;
        std::size_t last = kNone;
        // The slopes from the anchor's start that pass within tolerance of
        // every point merged into it.
        double lo = 0.0;
        double hi = 0.0;
    };
    std::vector<PassWrite> writes_;
    std::vector<Lane> lanes_;
};

// The label of a pass's one undo step.
inline constexpr const char* kAutomationWriteLabel = "Automation Write";

// The engine's live write `w` on document track `track`, in the lane's unit
// (dB clamped to the fader's range, pan to -1..1). False for a parameter
// with no lane or a value that is not finite: that write is skipped, not
// clamped, as std::clamp passes a NaN through.
bool live_pass_write(const AutomationWrite& w, model::Id track, PassWrite& out);

// A lane whose pass is left out because, thinned at up to 16 times its
// tolerance, it would still hold `points` points, past kMaxAutomationPoints.
struct DroppedLane {
    model::Id track = 0;
    model::AutomationParam param = model::AutomationParam::Volume;
    std::size_t points = 0;
};

// The pass as one thinned EditAutomation per written lane, in the order the
// lanes were first written. Where a later write covers an earlier one (a
// loop's next turn), the later wins. Outside what was written each lane
// keeps what it played: its old points in the gaps between runs, its old
// value (or, with no lane, the static fader or pan) on each run's edges,
// and a release ramps back to it over `return_frames`, as the audio did.
// Writes on a track the document does not hold are dropped. A lane that
// would pass kMaxAutomationPoints is thinned again at twice the tolerance,
// up to 4 times; one still over is left out and added to `dropped`, so the
// other lanes of the pass still land.
std::vector<model::EditAutomation> automation_pass_edits(const model::Snapshot& doc,
                                                         const std::vector<PassWrite>& writes,
                                                         model::Frames return_frames,
                                                         std::vector<DroppedLane>* dropped = nullptr);

// Removes the points of a run that lie within `tolerance` of the line
// between the points kept either side of them. The first and last points,
// and both points of a step, are always kept.
void thin_automation(std::vector<model::AutomationPoint>& points, float tolerance);

}  // namespace sonora
