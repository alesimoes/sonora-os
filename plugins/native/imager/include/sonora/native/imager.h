// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stereo imager (ADR-0011 N0, plugins.stock): the stereo width set
// separately in three bands. Narrowing the lows keeps a bass and a kick
// centred and mono-safe for vinyl-style or club playback; widening the highs
// opens a room or an overhead pair without moving the body of the mix.
//
// The bands come from Linkwitz-Riley fourth-order crossovers (S. H.
// Linkwitz, "Active crossover networks for noncoincident drivers", JAES
// 24(1), 1976): low and high sections sum to an all-pass, so with every
// width at 100 % the processor is flat in magnitude and changes only phase.
// The low band passes through the upper crossover's all-pass too, so the
// three bands stay aligned. Width is mid/side scaling of the side signal
// (Blumlein, British Patent 394,325, 1931). No latency.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_imager();

namespace imager {
enum : std::uint32_t {
    kBypass = 0,
    kLowXover,
    kHighXover,
    kWidthLow,
    kWidthMid,
    kWidthHigh,
    kParamCount,
};
}  // namespace imager

}  // namespace sonora::native
