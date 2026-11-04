// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-mixplan SESSION.sonora [gain|buses|parallel|space|all]
//
// Reads a session, measures its audio, and prints the plan the mix
// assistant's rules propose, with the diff accepting it would make. Changes
// nothing: the session file is only read.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "sonora/mix_planner.h"
#include "sonora/model/session_file.h"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: sonora-mixplan SESSION.sonora [gain|buses|parallel|space|all]\n");
        return 2;
    }
    sonora::MixPlanKind kind = sonora::MixPlanKind::All;
    if (argc == 3 && !sonora::parse_plan_kind(argv[2], kind)) {
        std::fprintf(stderr, "sonora-mixplan: \"%s\" is not gain, buses, parallel, space or all\n", argv[2]);
        return 2;
    }
    const std::string path = argv[1];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "sonora-mixplan: cannot open %s\n", path.c_str());
        return 1;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    sonora::model::Loaded loaded;
    sonora::model::LoadError load_error;
    if (!sonora::model::deserialize(bytes, loaded, load_error)) {
        std::fprintf(stderr, "sonora-mixplan: %s: %s\n", path.c_str(), load_error.text().c_str());
        return 1;
    }
    const std::size_t slash = path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);

    sonora::AnalysisHooks hooks;
    hooks.progress = [](double f) { std::fprintf(stderr, "\ranalysing %3.0f %%", f * 100.0); };
    sonora::MixAnalysis analysis;
    std::string error;
    if (!sonora::analyse_session(loaded.snapshot, dir, hooks, analysis, error)) {
        std::fprintf(stderr, "\nsonora-mixplan: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "\n");
    sonora::model::Plan plan;
    if (!sonora::propose_mix_plan(kind, loaded.snapshot, analysis, plan, error)) {
        std::fprintf(stderr, "sonora-mixplan: %s\n", error.c_str());
        return 1;
    }
    std::string text;
    if (!sonora::format_plan(loaded.snapshot, plan, text, error)) {
        std::fprintf(stderr, "sonora-mixplan: the plan does not apply: %s\n", error.c_str());
        return 1;
    }
    std::fputs(text.c_str(), stdout);
    return 0;
}
