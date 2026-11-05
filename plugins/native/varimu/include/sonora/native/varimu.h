// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vari-Mu: a variable-mu tube compressor (ADR-0011 N2, native.analog).
// Our own generic circuit values, held to the behaviour of the class of
// circuit, not to any product: a push-pull remote-cutoff gain cell whose
// grid bias is the control voltage, a rectifier and RC network making that
// voltage, and an optional output transformer.
//
//   input gain -> gain cell (remote-cutoff tube) -> output gain
//              -> iron -> mix with the dry input
//   input or key -> side-chain high-pass -> full-wave rectifier
//              -> [stereo link] -> attack/release network -> grid bias
//
// What the parameters mean, measured by tests/suite.cpp:
// - Threshold: the rectified level (peak, after the input gain) at which
//   the cell has given 2 dB of reduction. There is no ratio control: the
//   cell's remote-cutoff curve is the ratio. A tube's transconductance
//   falls as gm ~ (1 - u)^1.5 with the bias u (the three-halves law of a
//   space-charge-limited diode, Child-Langmuir, as in any triode
//   textbook), so the reduction in dB is -30 log10(1 - u): a soft knee,
//   about 1.3:1 near the threshold, 2:1 at 6 dB of reduction, 6:1 at
//   12 dB and nearly limiting past 20 dB, towards the cutoff where u
//   reaches its ceiling (40 dB of reduction).
// - Time constant: the six positions of the classic switch. 1-4 are fixed
//   attack/release pairs (0.2 ms/0.3 s, 0.2 ms/0.8 s, 0.4 ms/2 s,
//   0.8 ms/5 s). 5 and 6 are program dependent: a fast network (release
//   0.3 s or 0.8 s) and a slow one behind it (3 s or 8 s) that the fast
//   one charges, so a short peak releases fast and a long compression
//   releases slowly. Attack is the rectifier charging a capacitor on the
//   peaks only, so the 63 % time of a sine is a multiple of the nominal
//   tau (suite.cpp measures it).
// - Side-chain high-pass: two one-pole sections in the key, so bass does
//   not pump the programme.
// - Stereo link: how much of the louder channel's rectified level both
//   channels' networks see (100 %: one bias for both, as two cells on one
//   control line).
// - Iron: an output transformer, as a flux integrator that saturates on
//   low frequencies first (the core's flux is the integral of the
//   voltage, so for a given level it grows as 1/f) and a low-frequency
//   primary-inductance roll-off. A simple stage of ours, not a full
//   hysteresis model; the Jiles-Atherton core of common/dsp is the
//   follow-up (native.analog).
// - Quality: the rate the gain cell runs at. The cell's polynomial has
//   degree 3, so at 2x its harmonics of 20 kHz fall outside the band the
//   decimator keeps; the gain modulation's own sidebands are what remain.
//   Eco runs at the base rate with no latency. Reconfigures (ADR-0011 §1.5).
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_varimu();

namespace varimu {
enum : std::uint32_t {
    kBypass = 0,
    kInput,
    kThreshold,
    kTimeConstant,
    kScHpfOn,
    kScHpfFreq,
    kLink,
    kOutput,
    kIron,
    kMix,
    kExternalSc,
    kQuality,
    kParamCount,
};
constexpr std::uint32_t kFactors[] = {1, 2, 4};
}  // namespace varimu

}  // namespace sonora::native
