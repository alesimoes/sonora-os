// SPDX-License-Identifier: GPL-3.0-or-later
//
// The power supply behind a tube stage, for the hum it puts on the stage's
// output (ADR-0011 §3.3: "mains hum at 50 or 60 Hz with its harmonic
// series", from the circuit). Stage N2, slice 5.
//
// The circuit (the textbook capacitor-input supply of the power-supply
// chapters of the RCA Receiving Tube Manual and of M. Jones, *Valve
// Amplifiers*; values ours, in the range those give for a small
// amplifier):
//
//          +--D--+                 R1          R2
//   mains  |     |-- res --+------/\/\--+-----/\/\--+-- node (the stage's B+)
//   ~~~ Rw-+     |         |            |           |
//          +--D--+        C1  Iout     C2          C3  Ipre
//    (bridge, two diodes  |    |        |           |   |
//     in each half)      gnd  gnd      gnd         gnd gnd
//
// - The transformer secondary is a sine of `secondary_v_peak` behind the
//   winding resistance Rw; the bridge conducts through two diodes in
//   series on each half cycle (Shockley's law, Is and n).
// - The reservoir C1 feeds the output stage, a constant `i_output`, and
//   the two-section RC filter; the stage's node draws `i_preamp` (the
//   preamplifier, the modelled stage included) at its operating point.
//
// The ripple is full-wave: its fundamental is twice the mains frequency
// and its harmonics are the even harmonics of the mains, with the
// sawtooth-like spectrum of the reservoir's charge and discharge. The
// reservoir's waveform comes from integrating the bridge and the
// capacitors in time to their periodic steady state (not a sawtooth
// formula: the conduction angle sets the high harmonics). From the
// reservoir on, the circuit is linear at ripple amplitudes, and the
// filter's transfer, loaded by the stage's own supply admittance
// (TriodeStage::small_signal_supply), carries each harmonic to the stage.
//
// Checked against ngspice on the same circuit (plugins/native/measure,
// spice_noise.h): the reservoir's harmonics from a transient run of the
// bridge, the filter and stage's transfer from an AC run.
//
// Not real-time: prepare-time arithmetic only.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace sonora::dsp {

// The temperature every modelled noise source is at: ngspice's default
// (27 C), so the reference and the model agree to the bit on kT.
constexpr double kNoiseTemperatureK = 300.15;
constexpr double kBoltzmann = 1.380649e-23;        // J/K, SI 2019, exact
constexpr double kElectronCharge = 1.602176634e-19;  // C, SI 2019, exact

struct SupplyCircuit {
    double mains_hz = 50.0;
    double secondary_v_peak = 307.5;
    double r_winding = 50.0;
    // A silicon rectifier diode, Shockley's law: saturation current and
    // emission coefficient (no series resistance: Rw dominates).
    double diode_is = 1e-8;
    double diode_n = 1.9;
    double c_reservoir = 47e-6;
    double i_output = 0.040;
    double r_filter1 = 10e3, c_filter1 = 22e-6;
    double r_filter2 = 10e3, c_filter2 = 22e-6;
    double i_preamp = 0.002;
};

struct SupplyRipple {
    double ripple_hz = 0.0;  // twice the mains
    // Harmonic k of the ripple (k + 1 times ripple_hz) at the reservoir,
    // volts, with its phase against the mains sine (sin, from t = 0).
    std::vector<std::complex<double>> reservoir;
    double reservoir_dc = 0.0;  // mean reservoir voltage
    double node_dc = 0.0;       // and at the stage's node
    std::uint32_t max_newton = 0;  // the most iterations a step took
};

// Integrates the bridge, the reservoir and the filter (trapezoidal rule,
// `steps_per_period` steps per mains period, the diode solved by Newton)
// for `settle_periods` mains periods, then takes `harmonics` harmonics of
// the reservoir over the next period. Returns false if a step does not
// converge or the result is not finite.
bool simulate_supply(const SupplyCircuit& c, std::uint32_t harmonics, SupplyRipple& out,
                     std::uint32_t steps_per_period = 4000, std::uint32_t settle_periods = 40);

// The filter's transfer from the reservoir to the node, the node also
// loaded by `y_load` (siemens): the stage's supply admittance.
std::complex<double> supply_filter_transfer(const SupplyCircuit& c, double f_hz, std::complex<double> y_load);

}  // namespace sonora::dsp
