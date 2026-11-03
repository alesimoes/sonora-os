// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix bus (ADR-0011 §6, N2 slice 7, part 1): the bus compressor and the
// stereo width stage of the mix-bus insert (brain/research/
// native-plugin-suite.md §1.12, brain/research/native-mixbus-n2.md).
//
//   input -> bus compressor (stepped ratio, auto release, side-chain
//            high-pass, make-up, parallel mix) -> console colour
//            (oversampled line transformer) -> width (mid/side) -> output
//
// - The compressor is feed-forward in the log domain, after D. Giannoulis,
//   M. Massberg and J. D. Reiss, "Digital dynamic range compressor design --
//   a tutorial and analysis", JAES 60(6), 2012: the quadratic soft-knee gain
//   computer and the branching peak detector on the gain reduction, so the
//   static curve, attack and release measure as displayed. Fully linked: a
//   bus compressor must not move the stereo image, so both channels take
//   the louder one's reduction. Feedback (the detector on the output) is
//   not used: there, Giannoulis et al. show, the time constants and the
//   effective ratio depend on each other, and the ADR's ±10 % bar on the
//   displayed times could not be held (the research note has the algebra).
// - Ratios are the four stepped values of the classic bus practice (1.5, 2,
//   4, 10:1, research note §1.12), as a choice whose change ramps in the
//   log of the ratio, so it does not step the audio.
// - Auto release is program dependent, a design of ours on the same
//   detector: the reduction is the larger of a fast path (the set attack,
//   kAutoFastRelease) and a slow one that charges only on sustained
//   reduction (kAutoSlowAttack, kAutoSlowRelease). A transient recovers in
//   the fast time, a long stretch of compression in the slow one, without
//   pumping. Both figures are held by the suite.
// - Width scales the side signal, S = (L - R) / 2, by 0 to 200 %: 0 is
//   mono, 100 % is the input to the bit, 200 % doubles the side.
//
// - The colour (part 2) is the line output transformer of common/dsp
//   (transformer.h: a driver into a Jiles-Atherton core, with the supply's
//   sag), run oversampled after the compressor and mixed in by its amount
//   at the oversampled rate. 0 % is the compressor's output delayed by the
//   latency, to the bit. Quality is the oversampling factor (2, 4, 8) and
//   the latency follows from it; it is a non-automatable choice, applied at
//   the next prepare (ADR-0011 §1.5). The compressor's detector sees the
//   undelayed input, so the stage adds delay and no other change.
//
// Latency: the oversampler's round trip, reported through set_latency.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_mixbus();

namespace mixbus {
enum : std::uint32_t {
    kBypass = 0,
    kThreshold,
    kRatio,
    kKnee,
    kAttack,
    kRelease,
    kAutoRelease,
    kScHpfOn,
    kScHpfFreq,
    kMakeup,
    kMix,
    kWidth,
    // Part 2, appended: ids and indices of part 1 do not move.
    kQuality,
    kColour,
    kSag,
    kParamCount,
};

// The stepped ratios, in the order of the ratio parameter's choices.
constexpr double kRatios[] = {1.5, 2.0, 4.0, 10.0};
constexpr std::uint32_t kRatioCount = 4;

// Oversampling factor of each quality choice.
constexpr std::uint32_t kFactors[] = {2, 4, 8};

// Auto release: the fast path's release, and the slow path's charge and
// release, in seconds.
constexpr double kAutoFastRelease = 0.1;
constexpr double kAutoSlowAttack = 0.6;
constexpr double kAutoSlowRelease = 1.2;

// The ratio for a (smoothed) position on the choice: log-linear between
// adjacent steps, so a change of choice ramps without a step.
double ratio_at(double position) noexcept;
}  // namespace mixbus

}  // namespace sonora::native
