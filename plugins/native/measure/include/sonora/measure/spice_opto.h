// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SPICE reference for the optical cell and its circuit (common/dsp
// opto.h; ADR-0011 §3.3), made and kept honest the way spice.h makes the
// triode stage's:
// - The netlist is generated here from the values the model is built
//   from (OptoCircuit, OptoCellParams). The attenuator, the side chain's
//   C6, R30, R2, R37, C12 and R3 are ngspice elements with [M]'s values;
//   the photoresistor is a B-source conductance; the amplifier an ideal
//   gain, buffered from the tap (opto.h says why); the driver's swing a
//   min/max; the EL law and the photoconductor's three equations are
//   B-sources charging 1 F capacitors, the published equations verbatim.
//   So ngspice solves the same equations as the model, continuously and
//   simultaneously, with its own trapezoidal integration and step control.
//   What it checks is the model's discretisation: one linearised
//   backward-Euler step per sample, the side chain one sample behind.
// - Each case is a tone burst: quiet, then a step up for `hot_s`, then
//   quiet again for `release_s`, on a whole number of periods so the
//   steps fall on zero crossings. What is compared is the gain reduction
//   in dB, from the cell's carriers through the attenuator, on a time grid
//   that is fine (0.25 ms) for 40 ms after each step, where the attack and
//   the fast release are, and coarser after.
// - The netlists and the trajectories are committed in
//   plugins/native/opto/tests/spice/. The spice_opto_* ctests, where
//   ngspice is installed, regenerate each netlist and check it equals the
//   committed one byte for byte, re-run ngspice and check the trajectory.
//   The processor's suite reads the committed file, so it runs everywhere.
//
// Offline only: subprocesses and files, never an audio thread.
#pragma once

#include <string>
#include <vector>

#include "sonora/dsp/opto.h"
#include "sonora/json/json.h"

namespace sonora::measure {

struct OptoSpiceCase {
    std::string name;
    double limit;      // 0 compress, 1 limit
    double peak;       // peak reduction, 0..100
    double emphasis;   // 0..100
    double freq_hz;
    double quiet_dbfs, hot_dbfs;
    double quiet_s, hot_s, release_s;
};

std::vector<OptoSpiceCase> opto_spice_cases();
// The comparison grid, seconds.
std::vector<double> opto_spice_times(const OptoSpiceCase& c);
// The input in full-scale units at time t (the model's sample n is at n / fs).
double opto_case_input(const OptoSpiceCase& c, double t);
// Gain reduction in dB for free carriers n (OptoCell units).
double opto_gr_db(const dsp::OptoCircuit& k, const dsp::OptoCellParams& p, double limit, double n);
std::string opto_netlist(const OptoSpiceCase& c, const dsp::OptoCircuit& k, const dsp::OptoCellParams& p);
// The model's gain reduction at `times`: OptoChannel at `rate` from rest,
// linear between samples.
std::vector<double> opto_model_gr(const OptoSpiceCase& c, double rate, const std::vector<double>& times,
                                  const dsp::OptoCircuit& k, const dsp::OptoCellParams& p);

struct OptoSpiceReference {
    std::string ngspice;
    json::Value circuit, cell;
    std::vector<OptoSpiceCase> cases;
    std::vector<std::vector<double>> times, gr_db;
};
bool read_opto_spice_reference(const std::string& path, OptoSpiceReference& ref, std::string& error);
json::Value opto_circuit_json(const dsp::OptoCircuit& k);
json::Value opto_cell_json(const dsp::OptoCellParams& p);

// sonora-dsp-measure --spice-kind opto --spice-generate DIR / --spice-check DIR.
int opto_spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work);
int opto_spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                     int expected_cases);

}  // namespace sonora::measure
