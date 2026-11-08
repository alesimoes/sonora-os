// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze's subcommands (ADR-0066 §5). main.cpp owns the dispatch
// table and the signal handling; each pass owns its command in its own
// file (app/cmd_<pass>.cpp, or the stub until the pass lands).
//
// `args` are the words after the subcommand's name. `cancel` is set by
// SIGINT/SIGTERM and polled between blocks. Exit codes as ADR-0046:
// 0 report written, 1 the pass failed, 2 usage, 130 cancelled.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace sonora::analyze::app {

using CommandFn = int (*)(const std::vector<std::string>& args, const std::atomic<bool>& cancel);

int run_mastering_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel);
int run_key_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel);
int run_timbre_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel);

}  // namespace sonora::analyze::app
