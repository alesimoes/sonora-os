// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reverb (ADR-0011 §6, N2): an algorithmic reverberator on the FDN core of
// common/dsp (fdn.h), for a send or an insert.
//
//   input -> pre-delay -> 4 series Schroeder allpasses (diffusion)
//         -> 16-line FDN, Hadamard matrix, three-band T60 per line
//         -> width (mid/side) -> low cut, high cut -> mix with the dry
//
// What the parameters mean, measured by tests/suite.cpp:
// - Pre-delay: the first wet sample arrives exactly this long after the
//   dry one, at every size and rate. The reverb reports no latency: the
//   dry path is direct and the wet path is the effect, as with the delay.
// - Decay: the reverberation time T60 in the mid band, as ISO 3382-1
//   measures it (T30 from Schroeder backward integration, per octave
//   band). Low and high decay are multiples of it below and above their
//   crossovers (Jot and Chaigne, AES 1991); the decay does not change with
//   size, because each line's absorption follows its own length.
// - Size: the lines' lengths (25 to 83 ms at 100 %, 15 % of that at 0 %),
//   so the echo spacing and the modal density, not the decay.
// - Freeze: decay to infinity and the input shut, over 50 ms. The network
//   is lossless then (Schlecht and Habets, IEEE TSP 2017), so the tail
//   holds without growing.
//
// Size and pre-delay are not ramped: a change crossfades the old and the
// new taps over 30 ms (raised cosine), like the delay's times, so neither
// a pitch sweep nor a click is heard. Everything else is smoothed.
//
// Not yet (ADR-0011 N2 follow-ups, planning/BACKLOG.md): line modulation,
// a separate early-reflection stage, and calibration against measured
// rooms.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_reverb();

namespace reverb {
enum : std::uint32_t {
    kBypass = 0,
    kPredelay,
    kDecay,
    kSize,
    kDiffusion,
    kLowRatio,
    kLowFreq,
    kHighRatio,
    kHighFreq,
    kLowCut,
    kHighCut,
    kWidth,
    kFreeze,
    kMix,
    kParamCount,
};
constexpr double kMaxPredelayMs = 250.0;
// The allpass coefficient at 0 % and 100 % diffusion. Never 0: at g = 0 a
// Schroeder allpass is a plain delay, which would move the onset.
constexpr double kMinDiffusion = 0.1;
constexpr double kMaxDiffusion = 0.7;
}  // namespace reverb

}  // namespace sonora::native
