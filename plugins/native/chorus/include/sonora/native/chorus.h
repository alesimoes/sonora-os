// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chorus (ADR-0011 §6, N0): up to four voices, each a delay modulated by
// its own phase of one LFO (sine or triangle), read with third-order
// Lagrange interpolation; stereo spread offsets the right channel's phases;
// signed feedback; mix.
//
// J. Dattorro, "Effect Design Part 2: Delay-Line Modulation and Chorus",
// JAES 45(10), 1997; interpolation per T. I. Laakso et al., "Splitting the
// unit delay", IEEE SPM 13(1), 1996. No latency: the dry path is direct.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_chorus();

namespace chorus {
enum : std::uint32_t {
    kBypass = 0,
    kRate,
    kDepth,
    kDelay,
    kVoices,
    kSpread,
    kFeedback,
    kShape,
    kMix,
    kParamCount,
};
}  // namespace chorus

}  // namespace sonora::native
