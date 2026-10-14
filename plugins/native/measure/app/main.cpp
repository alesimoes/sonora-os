// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-dsp-measure: runs one processor's measurement suite.
//
//   sonora-dsp-measure --processor eq --quality plugins/native/eq/quality.json
//       --golden plugins/native/eq/tests/golden --report build/dsp-reports
//       [--regen-golden] [--cpu [--cpu-seconds 60] [--cpu-calibrate]]
//   sonora-dsp-measure --cpu-reference-kernel   (calibrate.cpp's figure)
//   sonora-dsp-measure --processor eq --host-fixture DIR --preroll 512
//       [--vst3-tempo 120] [--golden plugins/native/eq/tests/golden]
//   sonora-dsp-measure --spice-generate plugins/native/tube/tests/spice
//       --ngspice ngspice [--work DIR]
//   sonora-dsp-measure --spice-check plugins/native/tube/tests/spice
//       --ngspice ngspice --work DIR --case N --cases COUNT
//   the same two with --spice-kind opto and plugins/native/opto/tests/spice:
//       the optical cell's reference (measure/spice_opto.h); with
//       --spice-kind fet and plugins/native/fet/tests/spice: the FET
//       stage's (measure/spice_fet.h)
//
// Exit status 0 when every bar in quality.json is met. --host-fixture
// writes the reference renders the bundles are checked against through
// Sonora's LV2 and VST3 hosts instead (host_fixture.cpp).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sonora/measure/measure.h"
#include "sonora/measure/spice_fet.h"
#include "sonora/measure/spice_opto.h"

namespace sonora::native::suites {
int eq(const measure::Args&);
int compressor(const measure::Args&);
int gate(const measure::Args&);
int limiter(const measure::Args&);
int delay(const measure::Args&);
int chorus(const measure::Args&);
int reverb(const measure::Args&);
int tube(const measure::Args&);
int opto(const measure::Args&);
int fet(const measure::Args&);
}  // namespace sonora::native::suites

namespace sonora::measure {
int spice_generate(const std::string& dir, const std::string& ngspice, const std::string& work);
int spice_check(const std::string& dir, const std::string& ngspice, const std::string& work, int index,
                int expected_cases);
int host_fixture(const std::string& which, const std::string& dir, const std::string& golden_dir,
                 std::uint32_t preroll, double vst3_tempo);
}

int main(int argc, char** argv) {
    using namespace sonora;
    measure::Args args;
    std::string which;
    std::string fixture_dir;
    std::uint32_t preroll = 0;
    double vst3_tempo = 0.0;
    std::string spice_gen, spice_chk, ngspice = "ngspice", work = ".", spice_kind = "triode";
    int spice_case = -1, spice_cases = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--processor") which = next();
        else if (a == "--quality") args.quality_path = next();
        else if (a == "--golden") args.golden_dir = next();
        else if (a == "--report") args.report_dir = next();
        else if (a == "--regen-golden") args.regenerate_golden = true;
        else if (a == "--cpu") args.cpu = true;
        else if (a == "--cpu-seconds") args.cpu_seconds = std::atof(next().c_str());
        else if (a == "--cpu-calibrate") args.cpu_calibrate = true;
        else if (a == "--cpu-reference-kernel") {
            // The figure calibrate.cpp records for the reference machine.
            std::printf("reference kernel: %.2f us per block (p50)\n", measure::reference_kernel_us(60.0));
            return 0;
        }
        else if (a == "--host-fixture") fixture_dir = next();
        else if (a == "--preroll") preroll = static_cast<std::uint32_t>(std::atol(next().c_str()));
        else if (a == "--vst3-tempo") vst3_tempo = std::atof(next().c_str());
        else if (a == "--spice-generate") spice_gen = next();
        else if (a == "--spice-check") spice_chk = next();
        else if (a == "--ngspice") ngspice = next();
        else if (a == "--spice-kind") spice_kind = next();
        else if (a == "--work") work = next();
        else if (a == "--case") spice_case = std::atoi(next().c_str());
        else if (a == "--cases") spice_cases = std::atoi(next().c_str());
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    // An environment switch too, so a deliberate regeneration can go
    // through ctest unchanged (ADR-0011 §3.4: a deliberate commit).
    const char* regen = std::getenv("SONORA_REGEN_GOLDEN");
    if (regen != nullptr && std::strcmp(regen, "1") == 0) args.regenerate_golden = true;

    // The SPICE reference harness (measure/spice.h).
    if (spice_kind != "triode" && spice_kind != "opto" && spice_kind != "fet") {
        std::fprintf(stderr, "unknown --spice-kind '%s' (triode, opto, fet)\n", spice_kind.c_str());
        return 2;
    }
    if (!spice_gen.empty()) {
        if (spice_kind == "opto") return measure::opto_spice_generate(spice_gen, ngspice, work);
        if (spice_kind == "fet") return measure::fet_spice_generate(spice_gen, ngspice, work);
        return measure::spice_generate(spice_gen, ngspice, work);
    }
    if (!spice_chk.empty()) {
        if (spice_kind == "opto") return measure::opto_spice_check(spice_chk, ngspice, work, spice_case, spice_cases);
        if (spice_kind == "fet") return measure::fet_spice_check(spice_chk, ngspice, work, spice_case, spice_cases);
        return measure::spice_check(spice_chk, ngspice, work, spice_case, spice_cases);
    }
    if (!fixture_dir.empty()) {
        std::printf("sonora-dsp-measure: host fixtures for %s in %s\n", which.c_str(), fixture_dir.c_str());
        return measure::host_fixture(which, fixture_dir, args.golden_dir, preroll, vst3_tempo);
    }
    std::printf("sonora-dsp-measure: %s\n", which.c_str());
    if (which == "eq") return native::suites::eq(args);
    if (which == "compressor") return native::suites::compressor(args);
    if (which == "gate") return native::suites::gate(args);
    if (which == "limiter") return native::suites::limiter(args);
    if (which == "delay") return native::suites::delay(args);
    if (which == "chorus") return native::suites::chorus(args);
    if (which == "reverb") return native::suites::reverb(args);
    if (which == "tube") return native::suites::tube(args);
    if (which == "opto") return native::suites::opto(args);
    if (which == "fet") return native::suites::fet(args);
    std::fprintf(stderr, "unknown processor '%s'\n", which.c_str());
    return 2;
}
