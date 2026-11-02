// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <type_traits>

namespace sonora {

// What the audio thread applied of a written parameter (ADR-0027 §3), for
// Session::take_automation_writes(). A span of `frames` frames played at
// `value` from `frame`; a span that crosses the loop's end is two. `frames`
// 0 marks where a Touch parameter was released and began its return to the
// lane, over Session::touch_return_frames(). Trivial, as RingBuffer
// clears its slots with memset: value-initialise one, `AutomationWrite w{}`.
struct AutomationWrite {
    uint64_t pass;    // the roll it belongs to, counted from 1
    // The epoch of the plan its pass began on: a Load drops the passes
    // begun before it by this, as the pass number the Load reads can be
    // the one before a roll about to begin on the old document's plan.
    uint64_t epoch;
    // The document track the plan played on `track` (PlanTrackMix::track),
    // 0 for none: a reorder or a removal applied while rolling must not
    // file the writes stamped on the old plan under the track that now
    // holds the row when they are drained (#262 review N1).
    uint64_t track_id;
    uint64_t frame;   // timeline
    uint32_t frames;
    uint32_t track;   // engine track
    uint8_t param;    // PlanAutoParam
    float value;      // volume linear, as Track::gain(); pan -1 to +1
};

static_assert(std::is_trivially_copyable<AutomationWrite>::value, "RingBuffer memsets its slots");
// The ring holds 2 * 512 * (tracks + 1) of these, pre-faulted: 6.3 MB at 128
// tracks. A field more is that much more locked memory.
static_assert(sizeof(AutomationWrite) <= 48, "AutomationWrite grew");

}  // namespace sonora
