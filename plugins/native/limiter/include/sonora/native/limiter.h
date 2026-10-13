// SPDX-License-Identifier: GPL-3.0-or-later
//
// True-peak look-ahead limiter (ADR-0011 §6, N0), on
// common/dsp/limiter.h: 4x detection per ITU-R BS.1770-5 Annex 2 with a
// sinusoidal refinement between the 4x points, a sliding minimum over the
// look-ahead, and a triangular gain ramp complete before each peak. Then
// the true-peak guard (TruePeakGuard): 8x detection on that stage's output
// and a 1 ms look-ahead gain of its own, so the ceiling holds on the BS.1770
// meter and on a 16x reference whatever the first stage missed, full-band
// noise included (coordinator's decision on PR #34).
//
// Input gain (drive), ceiling in dBTP, release, look-ahead (a reconfiguring
// parameter: it sets the latency), true-peak or sample-peak detection,
// stereo link. Latency = look-ahead + the detector's 48 samples + the
// guard's 1 ms and its 8x alignment (about 262 samples at 48 kHz), reported
// exactly and unchanged by the detection switch.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_limiter();

namespace limiter {
enum : std::uint32_t {
    kBypass = 0,
    kInput,
    kCeiling,
    kRelease,
    kLookahead,
    kTruePeak,
    kLink,
    kParamCount,
};
}  // namespace limiter

}  // namespace sonora::native
