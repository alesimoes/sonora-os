// SPDX-License-Identifier: GPL-3.0-or-later
//
// Transformer (ADR-0011 §3.3, stage N2 slice 3): the iron of a line or
// output transformer as an insert, the colour an analog signal path adds
// when a transformer sits between a source and a load. The model is
// common/dsp's OutputStage with a stiff, effectively unclipped driver:
// a source resistance Rs feeding the primary, a load RL, and the
// magnetising branch as a Jiles-Atherton core, solved per sample. There is
// no oversampling: the flux is the integral of the voltage, so the core's
// nonlinearity is a function of a low-passed quantity and the harmonics it
// makes stand near the bass (tests/suite.cpp measures the aliasing).
//
//   input -> drive -> [Rs, core || RL] -> 1 / (drive g) -> output
//
// What the parameters mean:
// - Drive: 0 to 24 dB ahead of the circuit, divided out again after it, so
//   the small-signal gain is unity at every drive and drive only moves how
//   much of the signal's flux the core sees.
// - Core: three iron sizes. Each has a low-field corner frequency (where the
//   magnetising inductance takes half the current) and a saturation flux
//   (volt-seconds); a smaller core has a higher corner and saturates sooner.
//   The numbers are ours, generic, not a particular make. 1.0 FS stands for
//   100 V at the primary, so the saturation flux sets the frequency below
//   which full scale saturates the core: the flux is V / (2 pi f).
// - Output: a plain gain after the circuit.
//
// Not modelled here: leakage inductance and winding capacitance (the top end
// is flat), the driver's own distortion, and winding resistance's heating.
// Latency is zero in every configuration.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_transformer();

namespace transformer {
enum : std::uint32_t {
    kBypass = 0,
    kDrive,
    kCore,
    kOutput,
    kParamCount,
};
// The cores: low-field corner (Hz, with the circuit's Rs || RL across the
// winding) and saturation flux (V s). Shared with the suite, which computes
// the expected response and onset from them.
struct CoreSpec {
    const char* id;
    double corner_hz;
    double flux_vs;
};
constexpr CoreSpec kCores[] = {{"small", 30.0, 0.25}, {"line", 12.0, 0.6}, {"large", 5.0, 1.2}};
constexpr double kVoltsPerUnit = 100.0;
constexpr double kRSource = 600.0;
constexpr double kRLoad = 10000.0;
}  // namespace transformer

}  // namespace sonora::native
