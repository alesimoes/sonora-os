// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SPICE reference harness (ADR-0011 §3.3: the reference for a tube,
// opto, FET or tape stage is a SPICE simulation of the published circuit;
// stage N2).
//
// How a reference is made and kept honest:
// - The netlist is generated here from the same component and device
//   values the model in common/dsp is built from (triode.h), so the two
//   cannot describe different circuits. The device laws are written as
//   ngspice B-sources, the published equations verbatim.
// - ngspice (Debian trixie's, in tools/Dockerfile.plugins) runs each case
//   in batch mode with its default trapezoidal integration, tight
//   tolerances and a maximum step of 1/(2000 f0). The steady state is
//   reached after `settle_s`, and the harmonics are taken over the next
//   `window_s`, a whole number of periods, by integrating the output
//   against e^{-j 2 pi k f0 t} over ngspice's own (uneven) time points.
// - The netlists and the harmonics (complex, so the phase is held too)
//   are committed beside the processor that uses them
//   (plugins/native/tube/tests/spice/). The spice_* ctests, where ngspice
//   is installed, regenerate every netlist and check it equals the
//   committed one byte for byte, re-run ngspice, and check the harmonics
//   against the committed file. The model's own tests read the committed
//   file, so they run everywhere, ngspice or not.
// - Regenerating the references (`--spice-generate`) is a deliberate
//   commit, like a golden file (§3.4).
//
// Offline only: this runs a subprocess and touches files. It never runs on
// an audio thread.
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/dsp/triode.h"
#include "sonora/json/json.h"

namespace sonora::measure {

constexpr std::uint32_t kSpiceHarmonics = 9;

struct SpiceCase {
    dsp::TriodeLaw law;
    double amplitude_v;  // input sine, peak volts at vin
    double freq_hz;
};

struct SpiceTiming {
    double settle_s = 0.3;
    double window_s = 0.05;  // 5, 50 and 250 periods at 100, 1k and 5k
};

// The triode stage's reference cases: drive and frequency for the
// Dempwolf law (the one the processor runs), drive at 1 kHz for Koren's.
std::vector<SpiceCase> triode_spice_cases();
// "dempwolf-1v-1000hz"
std::string spice_case_name(const SpiceCase& c);
const char* law_name(dsp::TriodeLaw law);

// The netlist of one case. ngspice writes the operating point to op.txt
// and the output to out.txt, in its working directory.
std::string triode_netlist(const SpiceCase& c, const SpiceTiming& t, const dsp::TriodeStageCircuit& circuit,
                           const dsp::TriodeDevice& device);

using Harmonics = std::vector<std::complex<double>>;  // k = 1..kSpiceHarmonics

// From ngspice's wrdata output (time, value rows): the harmonics over
// [t0, t1], integrating the piecewise-linear waveform exactly.
bool spice_harmonics(const std::string& wrdata_path, double f0, double t0, double t1, Harmonics& out,
                     std::string& error);
// The same from uniform samples: y[n] is the value at time n / fs. The
// window [from, from + count) covers whole periods.
Harmonics sampled_harmonics(const std::vector<double>& y, double fs, double f0, std::size_t from,
                            std::size_t count);
// The model's harmonics for a case: TriodeStage at `rate`, from rest, the
// same sine, the same window.
Harmonics model_harmonics(const SpiceCase& c, const SpiceTiming& t, double rate,
                          const dsp::TriodeStageCircuit& circuit, const dsp::TriodeDevice& device,
                          bool tabulated = true, std::uint32_t* max_iterations = nullptr);

// The committed reference file, read.
struct SpiceReference {
    std::string ngspice;  // the version line that produced it
    SpiceTiming timing;
    std::vector<SpiceCase> cases;
    std::vector<Harmonics> harmonics;
    std::vector<std::vector<double>> op;  // v(a), v(g), v(k), v(p) per case
    json::Value circuit, device;          // as recorded
};
bool read_spice_reference(const std::string& path, SpiceReference& ref, std::string& error);
// The values a reference records, for comparison with the current model.
json::Value circuit_json(const dsp::TriodeStageCircuit& c);
json::Value device_json(const dsp::TriodeDevice& d);

// Command-line entry points (sonora-dsp-measure):
//   --spice-generate DIR --ngspice PATH       every netlist and reference.json
//   --spice-check DIR --ngspice PATH --case N --cases COUNT
//                                              case N against the committed files;
//                                              COUNT is how many cases the build
//                                              registered, so a new case cannot go
//                                              unchecked
// `work` is a scratch directory for ngspice's output.
int spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work);
int spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                int expected_cases);

}  // namespace sonora::measure
