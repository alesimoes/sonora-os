// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opto: an optical levelling amplifier (ADR-0011 §6, N2 slice 6), the
// electro-optical cell and the attenuator and side chain around it,
// modelled from the published circuit of a 1960s unit and from device
// physics (common/dsp opto.h, which cites both).
//
//   input -> attenuator (R6, R7, the cell's photoresistor)
//         -> gain -> mix with the dry input -> output
//   tap (R6/R7) or key -> side chain (C6 high-pass, peak reduction,
//         amplifier, pre-emphasis) -> [stereo link] -> driver -> EL panel
//         -> photoresistor
//
// What the parameters mean, measured by tests/suite.cpp:
// - Peak reduction: the side chain's gain, through an audio-taper control
//   as on the original. It sets the threshold and the amount together;
//   there is no threshold control. At 0 there is no compression.
// - Gain: the output level after the attenuator, the original's gain
//   control. It does not change the compression. 0 dB is unity with no
//   reduction (the original amplifier's 40 dB is normalised out).
// - Mode: compress (R7 shorted, the side chain sits across the cell: a
//   feedback loop, a low ratio) or limit (R7 in: the side chain partly
//   sees the input, a higher ratio). The switch is smoothed over 20 ms.
// - Side-chain emphasis: the pre-emphasis control, flat at 0 (the factory
//   setting); towards 100 the side chain loses up to 6 dB below about
//   160 Hz, so high frequencies are compressed more.
// - Stereo link: the two drivers' grids joined, as two units linked
//   through their stereo terminals.
// - Mix: dry against processed, for parallel compression (ours: the
//   original has none).
// - External key: the side chain listens to the key input instead of the
//   input, through the same attenuator share as the tap, as through a
//   second photoresistor matched to the first (the original lights one
//   for its meter). So the key is levelled the way the input would be:
//   the loop keeps its feedback character, and a key that is the input is
//   exactly the switch off, which is what a key the host routes nothing
//   to must be (ADR-0011 Amendment 2: Sonora's LV2 host connects the
//   input there, and the base keys from the input for VST3's inactive
//   bus).
// - Quality: the rate the attenuator and the cell run at. The gain moves
//   with the side chain's ripple, and the product of that movement with
//   the signal has components above Nyquist; at the base rate they fold
//   into the band. Eco runs at the base rate with no latency (aliasing at
//   the nominal level -71.1, -73.8 and -91.7 dBc at 44.1, 48 and 96 kHz,
//   against ADR-0011 §3.2's eco bar of -70; class A). Its level at high
//   frequencies depends on the tone's phase against the samples: the
//   light is a narrow pulse at each peak of the drive, sampled four times
//   a period at fs/4. A tone locked to fs/4 (12 kHz at 48 kHz) is reduced
//   within +-0.7 dB of standard's figure (1.2 dB of spread over its
//   phase), at 8 kHz within 0.07 dB; a tone not locked to the rate
//   averages over the phases, or near fs/4 beats slowly at its offset.
//   Not fixed in eco (tests/suite.cpp, eco_high_frequency, says why).
//   Standard (the default) runs at 4x through the half-band oversampler,
//   with its latency: 2x leaves the light's fourth harmonic folding into
//   the band at 44.1 kHz (-89 dBc at 19 kHz, over the -90 bar). High is
//   8x. Reconfigures (ADR-0011 §1.5), so not automatable.
//
// Attack and release are the cell's and are not parameters: fast attack,
// a two-stage release whose tail lengthens after longer and deeper
// reduction.
//
// Not yet (planning/BACKLOG.md): the original's tube amplifier and
// transformers (its colour; the triode stage of slice 2 at 2x would
// cost about class B), modelled noise and hum (slice 5).
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_opto();

namespace opto {
enum : std::uint32_t {
    kBypass = 0,
    kPeakReduction,
    kGain,
    kMode,
    kEmphasis,
    kLink,
    kMix,
    kExternalSc,
    kQuality,
    kParamCount,
};
// Oversampling factor of each quality choice.
constexpr std::uint32_t kFactors[] = {1, 4, 8};
}  // namespace opto

}  // namespace sonora::native
