// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-dsp-measure: runs one processor's measurement suite.
//
//   sonora-dsp-measure --processor eq --quality plugins/native/eq/quality.json
//       --golden plugins/native/eq/tests/golden --report build/dsp-reports
//       [--regen-golden] [--cpu [--cpu-seconds 60]]
//
// Exit status 0 when every bar in quality.json is met.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sonora/measure/measure.h"

namespace sonora::native::suites {
int eq(const measure::Args&);
int compressor(const measure::Args&);
int gate(const measure::Args&);
int limiter(const measure::Args&);
int delay(const measure::Args&);
int chorus(const measure::Args&);
}  // namespace sonora::native::suites

int main(int argc, char** argv) {
    using namespace sonora;
    measure::Args args;
    std::string which;
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
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    // An environment switch too, so a deliberate regeneration can go
    // through ctest unchanged (ADR-0011 §3.4: a deliberate commit).
    const char* regen = std::getenv("SONORA_REGEN_GOLDEN");
    if (regen != nullptr && std::strcmp(regen, "1") == 0) args.regenerate_golden = true;

    std::printf("sonora-dsp-measure: %s\n", which.c_str());
    if (which == "eq") return native::suites::eq(args);
    if (which == "compressor") return native::suites::compressor(args);
    if (which == "gate") return native::suites::gate(args);
    if (which == "limiter") return native::suites::limiter(args);
    if (which == "delay") return native::suites::delay(args);
    if (which == "chorus") return native::suites::chorus(args);
    std::fprintf(stderr, "unknown processor '%s'\n", which.c_str());
    return 2;
}
