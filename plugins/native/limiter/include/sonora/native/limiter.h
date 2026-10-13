// SPDX-License-Identifier: GPL-3.0-or-later
//
// True-peak look-ahead limiter (ADR-0011 §6, N0), on
// common/dsp/limiter.h: 4x detection per ITU-R BS.1770-5 Annex 2 with a
// sinusoidal refinement between the 4x points, a sliding minimum over the
// look-ahead, and a triangular gain ramp complete before each peak.
//
// Input gain (drive), ceiling in dBTP, release, look-ahead (a reconfiguring
// parameter: it sets the latency), true-peak or sample-peak detection,
// stereo link. Latency = look-ahead + the detector's 48 samples, reported
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
