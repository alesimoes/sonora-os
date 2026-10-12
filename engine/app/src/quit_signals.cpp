// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/quit_signals.h"

#include <csignal>

namespace sonora {
namespace {

volatile std::sig_atomic_t g_quit = 0;

void on_quit_signal(int) { g_quit = 1; }

}  // namespace

void install_quit_signals() {
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
    // sonora-session.service sets TTYVHangup=yes, and a hangup on tty1 at
    // its default action killed the engine with the take's headers unwritten.
    std::signal(SIGHUP, on_quit_signal);
}

bool quit_requested() { return g_quit != 0; }

void clear_quit_request() { g_quit = 0; }

}  // namespace sonora
