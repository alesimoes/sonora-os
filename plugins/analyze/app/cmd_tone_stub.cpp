// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built only while app/cmd_tone.cpp does not exist (ADR-0066 §5,
// ADR-0072): the subcommand is known, so the dispatch table is final, but
// it fails.
#include <cstdio>

#include "commands.h"

namespace sonora::analyze::app {

int run_tone_command(const std::vector<std::string>&, const std::atomic<bool>&) {
    std::fputs("sonora-analyze: tone: not implemented\n", stderr);
    return 1;
}

}  // namespace sonora::analyze::app
