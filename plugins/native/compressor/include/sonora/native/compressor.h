// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compressor (ADR-0011 §6, N0): feed-forward, log domain, after
// D. Giannoulis, M. Massberg and J. D. Reiss, "Digital dynamic range
// compressor design -- a tutorial and analysis", JAES 60(6), 2012: level in
// dB, quadratic soft-knee gain computer, gain reduction smoothed by the
// branching peak detector, so attack and release are independent of
// threshold and ratio and measure as set.
//
// Peak or RMS (10 ms) detection, a high-pass in the side-chain, an external
// key input, stereo link, make-up (manual or automatic) and a dry/wet mix
// for parallel compression. No look-ahead, so no latency.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_compressor();

namespace compressor {
enum : std::uint32_t {
    kBypass = 0,
    kThreshold,
    kRatio,
    kKnee,
    kAttack,
    kRelease,
    kMakeup,
    kAutoMakeup,
    kDetector,
    kScHpfOn,
    kScHpfFreq,
    kLink,
    kMix,
    kExternalSc,
    kParamCount,
};
}  // namespace compressor

}  // namespace sonora::native
