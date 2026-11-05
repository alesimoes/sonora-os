// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vocal Balance, the real-time half: a dynamic resonance suppressor with 3
// to 5 bands (ADR-0011 N5, "Vocal Balance (AI)"). The analysis that sets its
// bands is vocal_balance_analysis.h and runs offline; nothing here is a
// model. A band is explainable in three numbers: where it sits, how far its
// narrow level stands over the local level around it, and how much it cuts.
//
// Detection. Each band compares two band-passes of the input at its
// frequency: a narrow one (selectivity, the Q of what counts as a peak) and
// a wide one (Q 2, about a third of an octave each side). A peak adds the same power to both, so the power
// around it is the wide level less the narrow one. For a smooth spectrum
// the narrow level is the fraction r of the wide one that the ratio of the two filters
// white-noise power gains gives (about 2 / selectivity), so narrow / around is
// r / (1 - r); the band's excess is the narrow level over the around level
// in dB, less that: 10 log10(1 + peak / narrow-band noise) for a peak on
// white noise, and 0 dB for a smooth spectrum. It is floored at 30 dB
// of narrow over around (a pure tone). The reduction is
// the excess over the threshold, through a 6 dB soft knee, times depth,
// capped by the band's own cut, smoothed by attack and release, and linked
// across channels so the image does not move.
//
// The cut is a bell (Vicanek's matched design, as the EQ and the de-esser)
// at the band's frequency with Q = sharpness, run in series band after band.
// No latency: the detector listens to the input, the bell is the only thing
// in the audio path.
//
// Listen: delta is what the bands removed (input minus processed, scaled by
// mix); solo is the narrow detector bands summed, i.e. the regions being
// watched.
#pragma once

#include <cstdint>
#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_vocal_balance();

namespace vocal_balance {
constexpr std::uint32_t kMaxBands = 5;
enum : std::uint32_t {
    kBypass = 0,
    kBands,   // choice: 3, 4 or 5 active bands (the first n)
    kDepth,
    kSharpness,
    kSelectivity,
    kThreshold,
    kAttack,
    kRelease,
    kMix,
    kOutput,
    kListen,
    kFreq1,
    kFreq2,
    kFreq3,
    kFreq4,
    kFreq5,
    kCut1,
    kCut2,
    kCut3,
    kCut4,
    kCut5,
    kParamCount,
};
// The local-level band-pass: wide enough to stand for the spectrum around
// the peak, narrow enough that a voice's steep tilt (the strong low
// harmonics) does not fill it: at Q 0.7 a resonance at 6 kHz was measured
// against the energy of 3 kHz and read far too small.
constexpr double kWideQ = 2.0;
}  // namespace vocal_balance

}  // namespace sonora::native
