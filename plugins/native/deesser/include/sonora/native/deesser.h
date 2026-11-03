// SPDX-License-Identifier: GPL-3.0-or-later
//
// De-esser: a sibilance compressor (ADR-0011 §6, N0).
//
// Band mode cuts a bell filter (the cookbook peaking section, matched
// design: M. Vicanek, "Matched Second Order Digital Filters", 2016) at the
// sibilance frequency by the reduction the detector asks for. The detector
// is a band-pass of the same frequency and width; its output, a
// sine-calibrated RMS level (a sine at the centre reads its peak level),
// goes through the Giannoulis et al. gain computer (JAES 60(6), 2012) and
// the branching smoother, so attack and release measure as set.
//
// Why a bell and not a crossover. A Linkwitz-Riley split (JAES 24(1), 1976)
// with the high leg reduced sums to an all-pass when nothing is reduced,
// which turns the phase and reads as 3 to 5 samples of latency on the
// impulse (measured). Subtracting the high leg from the input instead
// leaves the reduction ineffective, since the leg leads in phase (measured:
// about 3 dB of an intended 20). The bell is the identity at zero
// reduction, with no latency, and cuts the full amount at its centre.
//
// Wide mode applies the same reduction to the whole signal, per sample,
// which is the older, cruder de-esser and what some singers want. The range
// caps the reduction. Listen outputs the detector's band.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_deesser();

namespace deesser {
enum : std::uint32_t {
    kBypass = 0,
    kMode,
    kFrequency,
    kQ,
    kThreshold,
    kRatio,
    kKnee,
    kRange,
    kAttack,
    kRelease,
    kLink,
    kOutput,
    kListen,
    kParamCount,
};
}  // namespace deesser

}  // namespace sonora::native
