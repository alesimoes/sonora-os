// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built only while app/cmd_vocal.cpp does not exist (ADR-0066 §5,
// ADR-0104): the subcommand is known, so the dispatch table is final. It
// already answers a missing file with the usage line (exit 2), as the real
// command will; any other call fails.
#include <cstdio>

#include "commands.h"

namespace sonora::analyze::app {

int run_vocal_balance_command(const std::vector<std::string>& args, const std::atomic<bool>&) {
    if (args.empty()) {
        std::fputs("usage: sonora-analyze vocal-balance <vocal.wav> [--target-level-db N] "
                   "[--out report.json]\n",
                   stderr);
        return 2;
    }
    std::fputs("sonora-analyze: vocal-balance: not implemented\n", stderr);
    return 1;
}

}  // namespace sonora::analyze::app
