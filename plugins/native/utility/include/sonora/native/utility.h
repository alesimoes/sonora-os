// SPDX-License-Identifier: GPL-3.0-or-later
//
// Utility (ADR-0011 N0, plugins.stock): gain, polarity per channel, left/
// right swap, balance, mid/side width, mono sum and a 5 Hz DC blocker. The
// housekeeping a mix needs at the head of a chain and on a bus: flipping a
// phase-inverted microphone, checking a mix in mono, trimming a level.
//
// Every switch is a smoothed weight, so flipping one is a 10 ms crossfade
// and never a click. At its defaults the processor is bit-exact. No latency.
// Mid/side: A. D. Blumlein, British Patent 394,325 (1931).
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_utility();

namespace utility {
enum : std::uint32_t {
    kBypass = 0,
    kGain,
    kPolarityL,
    kPolarityR,
    kSwap,
    kBalance,
    kWidth,
    kMono,
    kDcBlock,
    kParamCount,
};
}  // namespace utility

}  // namespace sonora::native
