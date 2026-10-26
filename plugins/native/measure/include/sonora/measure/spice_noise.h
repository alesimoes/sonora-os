// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SPICE references for the tube stage's modelled noise and hum
// (ADR-0011 §3.3, slice 5), on the same principle as spice.h: netlists
// generated from the values the model is built from (triode.h, supply.h),
// run by ngspice, committed with their results beside the processor
// (plugins/native/tube/tests/spice-noise/), re-run and compared by the
// spice_noise_* ctests where ngspice is installed; the model's own checks
// read the committed file, so they run everywhere.
//
// The cases:
// - "noise": ngspice's small-signal noise analysis (.noise) of the stage
//   at its operating point, the output noise density across 10 Hz to
//   40 kHz. Every resistor carries its thermal noise (ngspice's own); the
//   triode's shot noise is a resistor of Req = 2.5 / gm, the model's
//   figure, whose voltage an E source puts in series with the grid inside
//   the device laws (the B-sources read v(gi, k), not v(g, k)), so it
//   drives none of the electrode capacitances, as in the model.
// - "supply-50hz", "supply-60hz": a transient run of the bridge, the
//   reservoir and the filter, with the loads as constant currents; the
//   reservoir's ripple harmonics (of twice the mains), complex.
// - "chain-50hz", "chain-60hz": an AC run from the reservoir, through the
//   filter, into the stage at its operating point (B+ at the model's
//   supply voltage), to the load: the transfer at each ripple harmonic.
//   The stage loads the filter here exactly as the circuit does.
//
// Offline only.
#pragma once

#include <complex>
#include <string>
#include <vector>

#include "sonora/dsp/supply.h"
#include "sonora/dsp/triode.h"
#include "sonora/json/json.h"

namespace sonora::measure {

enum class NoiseSpiceKind { Noise, Supply, Chain };

struct NoiseSpiceCase {
    std::string name;
    NoiseSpiceKind kind;
    double mains_hz;  // Supply, Chain
};

struct NoiseSpiceTiming {
    double settle_s = 0.6;
    double window_s = 0.1;  // whole ripple periods at 100 and 120 Hz
};

std::vector<NoiseSpiceCase> noise_spice_cases();

std::string noise_spice_netlist(const NoiseSpiceCase& c, const NoiseSpiceTiming& t,
                                const dsp::TriodeStageCircuit& circuit, const dsp::TriodeDevice& device,
                                const dsp::SupplyCircuit& supply);

struct NoiseSpiceResult {
    std::vector<double> freq;                // Noise: Hz; Chain: Hz
    std::vector<double> density;             // Noise: V/sqrt(Hz) at the load
    std::vector<std::complex<double>> value;  // Supply: harmonics (k = 1..); Chain: transfer
};

struct NoiseSpiceReference {
    std::string ngspice;
    NoiseSpiceTiming timing;
    json::Value circuit, device, supply;
    std::vector<NoiseSpiceCase> cases;
    std::vector<NoiseSpiceResult> results;
};
bool read_noise_spice_reference(const std::string& path, NoiseSpiceReference& ref, std::string& error);
json::Value supply_json(const dsp::SupplyCircuit& s);

// The model's side of each case, for the suite and for the generator's
// printout: the stage's output noise density (V/sqrt(Hz)) at f; the
// reservoir's ripple harmonics; the reservoir-to-load transfer at f.
double model_noise_density(const dsp::TriodeStage& st, double f_hz);
std::complex<double> model_chain_transfer(const dsp::TriodeStage& st, const dsp::SupplyCircuit& s, double f_hz);

int noise_spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work);
int noise_spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                      int expected_cases);

}  // namespace sonora::measure
