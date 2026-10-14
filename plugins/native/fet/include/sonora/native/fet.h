// SPDX-License-Identifier: GPL-3.0-or-later
//
// FET: a FET peak limiter (ADR-0011 §6, N2 slice 6b), the gain-reduction
// stage and side chain of a 1960s unit's published circuit: a JFET as the
// bottom leg of a voltage divider, its gate linearised by the drain's own
// signal, a feedback side chain with a full-wave rectifier charging the
// control capacitor (common/dsp fet.h, which cites the sources).
//
//   input -> input gain -> R5 / FET divider -> preamplifier -> output gain
//         -> mix with the dry input -> output
//   preamplifier output (or the key) -> ratio tap -> amplifier ->
//         rectifier -> attack network -> C22 -> [stereo link] -> FET gate
//
// What the parameters mean, measured by tests/suite.cpp:
// - Input: the level into the divider, which sets how far over the
//   threshold the signal is (the original's input control). It raises the
//   output too, as on the original; Output makes the level up.
// - Output: the level after the preamplifier. 0 dB in and out is unity
//   below the threshold.
// - Attack: the attack network's time constant, (R54 + R55) C22, 0.10 to
//   5.6 ms: R55's travel. The gain reduction's own attack is shorter and
//   depends on the step, because the rectifier drives C22 far past the
//   level it settles at: on the suite's step (a 10 kHz burst to 0 dBFS,
//   about 11 dB of reduction at 20:1) its t63 runs from about 15 us to
//   about 270 us across the travel ([M]: 20 to 800 us). tests/suite.cpp,
//   attack().
// - Release: the release network's time constant, (R57 + R56) C22, 59 ms
//   to 1.16 s ([M]: 50 ms to 1.1 s). The gate follows C22 half at once and
//   half through C9 over about a second (the LN circuit's coupling), so
//   the reduction recovers in two stages, and the second, which this
//   control does not set, dominates at depth: from 11 dB at the fastest
//   setting the reduction halves in about 0.36 s. As the published values
//   give it; Unverified against hardware (the research note).
// - Ratio: the switch, 4, 8, 12, 20 or all buttons in. Each position
//   moves the side chain's tap and the rectifiers' bias, so a higher ratio
//   also sets a higher threshold [M]. The ratios are the circuit's, from
//   the published values, and come out under the labels: at their
//   steepest 3.4, 6.1, 9.4 and 18.1 to 1. All buttons in is the circuit
//   with every switch closed as we read the figure, not a preset curve:
//   it lowers the threshold by 12 dB below 4:1's at about 3.6 to 1, where
//   [M] describes "between 12:1 and 20:1" (the research note: Unverified).
//   Smoothed over 20 ms.
// - Stereo link: the two control nodes joined, as two units joined
//   through the stereo adapter (which doubles the capacitance one channel
//   drives: [M]'s fastest attack doubling when linked).
// - Mix: dry against processed, for parallel compression (ours: the
//   original has none).
// - External key: the side chain listens to the key through a second FET
//   divider on the same gate, as matched to the first; a key that is the
//   input is the switch off, which is what a key the host routes nothing
//   to must be (ADR-0011 Amendment 2).
// - Quality: the rate the divider and side chain run at. The gain moves
//   with the rectifier's ripple, faster the faster the attack, and the
//   product with the signal has components above Nyquist. Eco runs at the
//   base rate with no latency; standard (the default) at 4x through the
//   half-band oversampler, with its latency; high at 8x. Reconfigures
//   (ADR-0011 §1.5), so not automatable. Eco is the economy mode, with
//   two limits the suite holds and the research note records: its
//   rectifier sees a high tone only at the samples, so near fs/4 it
//   under-reads the peaks between them (a tone locked to fs/4 is reduced
//   up to 2.5 dB less than standard reduces it, depending on its phase);
//   and with fast attack and release into heavy limiting it aliases far
//   more than standard (20:1, input +18 dB, fastest attack and release,
//   48 kHz: -34 dBc, against -75 dBc in standard).
//   Standard is the mode for limiting high-frequency peaks.
//
// Not yet (planning/BACKLOG.md): the original's class-A output stage and
// transformers (the colour the manual credits them with), modelled noise
// and hum (slice 5).
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_fet();

namespace fet {
enum : std::uint32_t {
    kBypass = 0,
    kInput,
    kOutput,
    kAttack,
    kRelease,
    kRatio,
    kLink,
    kMix,
    kExternalSc,
    kQuality,
    kParamCount,
};
// Oversampling factor of each quality choice.
constexpr std::uint32_t kFactors[] = {1, 4, 8};
}  // namespace fet

}  // namespace sonora::native
