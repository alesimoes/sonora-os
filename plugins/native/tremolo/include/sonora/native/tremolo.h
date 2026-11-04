// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tremolo and auto-pan (ADR-0011 §6, N0, modulation family), one processor
// with two modes.
//
// Tremolo: the gain follows a unipolar LFO, g = 1 - depth (1 - u) / 2 with
// u in [-1, 1] the LFO, so the signal swings between unity and 1 - depth and
// depth 0 is bit-exact. The right channel's LFO is offset by the spread (100 %
// is half a cycle, so the channels sum to a constant gain on a symmetric
// shape).
//
// Auto-pan: the LFO moves a pan position p = depth u in [-1, 1] with the
// constant-power sin/cos law (Blumlein), gL = sqrt2 cos(pi (p + 1) / 4),
// gR = sqrt2 sin(pi (p + 1) / 4): unity at the centre, gL^2 + gR^2 = 2.
// Applied as a balance, each channel keeping its own signal.
//
// Shapes: sine, triangle, and a soft square tanh(4 sin) / tanh(4), smooth
// enough that the gain never steps. No latency: the signal path is a gain.
// Every parameter is smoothed.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_tremolo();

namespace tremolo {
enum : std::uint32_t {
    kBypass = 0,
    kMode,
    kRate,
    kDepth,
    kShape,
    kSpread,
    kParamCount,
};
}  // namespace tremolo

}  // namespace sonora::native
