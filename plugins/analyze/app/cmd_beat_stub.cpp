// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built only while app/cmd_beat.cpp does not exist (ADR-0066 §5, ADR-0114):
// the subcommand is known, so the dispatch table is final, but it fails.
#include <cstdio>

#include "commands.h"

namespace sonora::analyze::app {

int run_beat_command(const std::vector<std::string>&, const std::atomic<bool>&) {
    std::fputs("sonora-analyze: beat: not implemented\n", stderr);
    return 1;
}

}  // namespace sonora::analyze::app
