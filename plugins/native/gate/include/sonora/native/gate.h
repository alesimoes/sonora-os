// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gate and downward expander (ADR-0011 §6, N0).
//
// Gate: opens when the key's envelope reaches the threshold, closes when it
// falls below threshold minus hysteresis and the hold time has run out
// (separate open and close thresholds, hold, range: J. Reiss and
// A. McPherson, *Audio Effects*, CRC, 2014, ch. 6). The envelope is a peak
// follower with a fixed 10 ms fall, so a low note does not chatter the gate
// at every zero crossing.
//
// Expander: the Giannoulis et al. gain computer mirrored below threshold,
// with a ratio and a soft knee, its attenuation limited to the range.
//
// In both, the attenuation in dB is smoothed by the branching detector:
// rising attenuation (closing) with the release time, falling (opening)
// with the attack time. Key filters, an external key, stereo link, and a
// key-listen switch. No look-ahead, so no latency.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_gate();

namespace gate {
enum : std::uint32_t {
    kBypass = 0,
    kMode,
    kThreshold,
    kRange,
    kRatio,
    kKnee,
    kAttack,
    kHold,
    kRelease,
    kHysteresis,
    kKeyHpfOn,
    kKeyHpfFreq,
    kKeyLpfOn,
    kKeyLpfFreq,
    kLink,
    kExternalSc,
    kListen,
    kParamCount,
};
}  // namespace gate

}  // namespace sonora::native
