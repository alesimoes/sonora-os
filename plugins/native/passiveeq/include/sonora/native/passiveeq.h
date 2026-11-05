// SPDX-License-Identifier: GPL-3.0-or-later
//
// Passive EQ (ADR-0011 §3.3, N2 analog family): the boost-and-cut network
// of a passive program equaliser, with the make-up stage's iron optional.
// The architecture is the published one, the values are ours and generic
// (not a particular unit's):
//
//   input -> LF boost shelf -> LF cut shelf -> HF boost (bell, variable
//            bandwidth) -> HF cut shelf -> [drive -> core -> 1/drive] -> output
//
// What makes the passive network what it is, and what the suite measures:
// - The low-frequency boost and cut are two separate paths that both act at
//   once. The cut shelf's corner sits above the boost's (kCutRatio) and
//   reaches only kCutScale of its knob, so equal settings give a bump at
//   the bottom and a dip above it, not a flat line. That interaction is the
//   reason the network is used, so it is part of the model.
// - The high boost is a resonance whose bandwidth is a knob; the high cut is
//   a shelf. The two are independent.
// - Each section is a matched second-order design (common/dsp/biquad.h,
//   Vicanek 2016), run as a trapezoidal SVF whose coefficients ramp over
//   each control block, so knobs never zipper.
// - Iron: off, the whole processor is linear. On, the output goes through
//   the large core of common/dsp's OutputStage (the transformer processor's
//   Jiles-Atherton loop, 5 Hz corner so the bass is not rolled off), with
//   drive as the transformer's: unity small-signal gain at any drive.
//   The core is not oversampled (the flux is a low-passed quantity; the
//   transformer suite measures the aliasing).
// Latency is zero in every configuration.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_passiveeq();

namespace passiveeq {
enum : std::uint32_t {
    kBypass = 0,
    kLfBoost,
    kLfAtten,
    kLfFreq,
    kHfBoost,
    kHfQ,
    kHfFreq,
    kHfAtten,
    kHfAttenFreq,
    kIron,
    kDrive,
    kOutput,
    kParamCount,
};
constexpr double kLfFreqs[] = {20.0, 30.0, 60.0, 100.0};
constexpr double kHfFreqs[] = {3000.0, 4000.0, 5000.0, 8000.0, 10000.0, 12000.0, 16000.0};
constexpr double kHfAttenFreqs[] = {5000.0, 10000.0, 20000.0};
constexpr double kCutRatio = 2.5;  // cut shelf corner over boost shelf corner
constexpr double kCutScale = 0.7;  // cut shelf depth over its knob
constexpr double kLfBoostQ = 0.6;
constexpr double kLfCutQ = 0.7;
constexpr double kHfCutQ = 0.7071067811865476;
}  // namespace passiveeq

}  // namespace sonora::native
