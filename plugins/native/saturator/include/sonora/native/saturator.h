// SPDX-License-Identifier: GPL-3.0-or-later
//
// Saturator and exciter (ADR-0011 §6, the core stock set): an oversampled
// static waveshaper with drive, tone, mix and output. It is the clean,
// character-free end of the distortion family: the curve is a stated
// function (tests/suite.cpp measures the output's harmonics against that
// function's own Fourier series), where Tube and Tape model a circuit and
// a medium.
//
//   input -> [exciter: split highs] -> drive -> oversampler up
//         -> curve -> DC blocker -> oversampler down -> 1/drive
//         -> tone -> mix with the (latency-matched) dry -> output
//
// What the parameters mean:
// - Drive: 0 to 36 dB ahead of the curve, divided out again after it, so
//   the small-signal gain is unity at every drive and drive only moves
//   where the curve bends. A loud signal therefore comes out quieter as
//   drive rises (it clips at 1 / drive); Output makes it up.
// - Shape: soft (tanh, odd harmonics, smooth), warm (tanh around a bias of
//   0.25, so the even harmonics lead at low level; its DC is removed), hard
//   (u - u^3/3 up to 1, flat beyond: the strongest high harmonics).
// - Mode: saturate (the whole signal goes through the curve) or exciter
//   (only the part above Focus does, and the result replaces that part:
//   below Focus the signal is untouched, and at small signals the whole
//   processor is the identity).
// - Focus: the exciter's split frequency, a pair of one-pole high-passes
//   (12 dB per octave). Unused in saturate mode.
// - Tone: a shelf on what the curve adds (the whole wet signal in saturate
//   mode): +-12 dB above 1.2 kHz, flat below, first order.
// - Mix: dry to wet in saturate mode; the amount of the exciter's added
//   part in exciter mode.
// - Quality: the oversampling of the curve (2x, 4x, 8x), with its
//   latency. Reconfigures.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_saturator();

namespace saturator {
enum : std::uint32_t {
    kBypass = 0,
    kDrive,
    kShape,
    kMode,
    kFocus,
    kTone,
    kMix,
    kOutput,
    kQuality,
    kParamCount,
};
constexpr std::uint32_t kFactors[] = {2, 4, 8};
// The tone shelf's corner, and the warm curve's bias: shared with the suite,
// which computes the expected response and harmonics from them.
constexpr double kToneHz = 1200.0;
constexpr double kWarmBias = 0.25;
// The DC blocker after the curve (Hz).
constexpr double kDcHz = 5.0;
}  // namespace saturator

}  // namespace sonora::native
