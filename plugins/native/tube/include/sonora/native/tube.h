// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tube drive (ADR-0011 §6, N2): one common-cathode high-mu triode stage,
// solved from the circuit at an oversampled rate (common/dsp triode.h).
//
//   input -> drive -> oversample (2x/4x/8x) -> triode stage -> decimate
//         -> normalise and re-invert -> auto level, output -> mix with
//            the dry signal, delayed to match
//
// What the parameters mean, measured by tests/suite.cpp:
// - Drive: the volts at the stage's input per unit of full scale, in dB:
//   at 0 dB a full-scale sine is 1 V peak at vin. The harmonic profile at
//   each drive and frequency is the circuit's, held to an ngspice
//   simulation of the same netlist (tests/spice/).
// - Auto level: divides the output by the drive, so a clean signal comes
//   out at the level it went in whatever the drive; heavier drive then
//   reads as more distortion, not as more level. Off, the drive is also
//   an input gain.
// - Output, mix: after the stage. The dry signal is delayed by the
//   oversampler's latency, so mixing never combs.
// - Quality: the oversampling factor. Reconfigures and changes latency
//   (ADR-0011 §1.5), so it is not automatable.
//
// The stage inverts, as every common-cathode stage does; the processor
// inverts back, so a mix of dry and processed never cancels.
//
// Its output is normalised by the stage's small-signal gain at 1 kHz, so
// unity drive with auto level on is unity gain for a quiet signal.
//
// - Noise, hum (slice 5): the stage's own thermal and shot noise and the
//   supply's ripple at its output, each from the circuit (common/dsp
//   triode.h, small_signal_noise; supply.h), at the modelled level at
//   100 % and digitally silent at 0 %, the default until the owner rules
//   on ADR-0011 "For the owner" item 7. The noise is added after the drive
//   gain, at the stage's input, so the drive does not scale it but the
//   stage's response and auto level act on it as on the signal; the hum enters at the load. Both are in the processed
//   path, so mix scales them. Mains (50 or 60 Hz) sets the ripple's
//   frequencies; it reconfigures (the supply is integrated in prepare()).
//
// Not yet (ADR-0011 N2, planning/BACKLOG.md): the output transformer,
// power-supply sag (a single preamp stage draws too little for it to
// matter; it belongs with the output stage), the bias as a parameter,
// flicker noise and heater hum (slice 5 follow-ups).
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_tube();

namespace tube {
enum : std::uint32_t {
    kBypass = 0,
    kDrive,
    kOutput,
    kMix,
    kAutoLevel,
    kQuality,
    kNoise,
    kHum,
    kMains,
    kParamCount,
};
// Oversampling factor of each quality choice.
constexpr std::uint32_t kFactors[] = {2, 4, 8};
}  // namespace tube

}  // namespace sonora::native
