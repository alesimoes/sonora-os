// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SPICE reference for the FET gain-reduction stage and its side chain
// (common/dsp fet.h; ADR-0011 §3.3), made and kept honest the way
// spice.h and spice_opto.h make theirs:
// - The netlist is generated here from the values the model is built from
//   (FetCircuit, JfetParams, DiodeParams). R5 and the FET are ngspice
//   elements: the FET is ngspice's own NJF level 1 with [J]'s BETA, VTO
//   and LAMBDA, and the rectifiers are ngspice's own diode model with
//   [D]'s IS and N, so the device laws are ngspice's implementations, not
//   ours. C22, C9 with R6 + R7, C27 across R54, R55 and the release
//   resistor are ngspice elements with [M]'s values. What is behavioural:
//   the gate (the LN circuit's midpoint, (Vc + Vq) / 2 + vd / 2, bounded at
//   the gate's forward limit), the side chain's gain and swing and its
//   phase splitter (the rectifier anodes at Vb +- s), and the rest point
//   as a DC source at the value the model solves for. So ngspice solves
//   the same circuit as the model, continuously, with its own trapezoidal
//   integration and step control; what it checks is the model's
//   discretisation and its solve of the JFET and the rectifier.
// - Each case is a tone burst: quiet, a step up for `hot_s`, quiet again
//   for `release_s`, on whole periods so the steps fall on zero
//   crossings. What is compared is the gain reduction in dB, from the
//   gate's state, on a time grid fine (10 us) for 2 ms after each step,
//   where the attack is, and coarser after.
// - The netlists and the trajectories are committed in
//   plugins/native/fet/tests/spice/. The spice_fet_* ctests, where ngspice
//   is installed, regenerate each netlist and check it equals the
//   committed one byte for byte, re-run ngspice and check the trajectory.
//   The processor's suite reads the committed file, so it runs everywhere.
//
// Offline only: subprocesses and files, never an audio thread.
#pragma once

#include <string>
#include <vector>

#include "sonora/dsp/fet.h"
#include "sonora/json/json.h"

namespace sonora::measure {

struct FetSpiceCase {
    std::string name;
    double ratio;    // switch position, 0..4 (fet.h)
    double r55, r56;  // attack and release resistors, ohms
    double freq_hz;
    double quiet_dbfs, hot_dbfs;  // at the input control's 0 dB
    double quiet_s, hot_s, release_s;
};

std::vector<FetSpiceCase> fet_spice_cases();
// The comparison grid, seconds.
std::vector<double> fet_spice_times(const FetSpiceCase& c);
// The input in full-scale units at time t (the model's sample n is at n / fs).
double fet_case_input(const FetSpiceCase& c, double t);
// Gain reduction in dB for a gate base voltage (the ohmic channel).
double fet_gr_db(const dsp::FetCircuit& k, const dsp::JfetParams& j, double vg0);
std::string fet_netlist(const FetSpiceCase& c, const dsp::FetCircuit& k, const dsp::JfetParams& j,
                        const dsp::DiodeParams& d);
// The model's gain reduction at `times`: FetChannel at `rate` from rest,
// linear between samples.
std::vector<double> fet_model_gr(const FetSpiceCase& c, double rate, const std::vector<double>& times,
                                 const dsp::FetCircuit& k, const dsp::JfetParams& j, const dsp::DiodeParams& d);

struct FetSpiceReference {
    std::string ngspice;
    json::Value circuit, devices;
    std::vector<FetSpiceCase> cases;
    std::vector<std::vector<double>> times, gr_db;
};
bool read_fet_spice_reference(const std::string& path, FetSpiceReference& ref, std::string& error);
json::Value fet_circuit_json(const dsp::FetCircuit& k);
json::Value fet_devices_json(const dsp::JfetParams& j, const dsp::DiodeParams& d);

// sonora-dsp-measure --spice-kind fet --spice-generate DIR / --spice-check DIR.
int fet_spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work);
int fet_spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                    int expected_cases);

}  // namespace sonora::measure
