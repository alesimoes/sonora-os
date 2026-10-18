// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/quit_signals.h"

#include <atomic>
#include <csignal>

namespace sonora {
namespace {

// Atomic, not volatile sig_atomic_t: the kernel runs the handler on any
// thread that does not block the signal, and main polls from its own. A
// lock-free atomic is the one shared type a handler may write (C++17
// [support.signal]).
std::atomic<int> g_quit{0};
static_assert(std::atomic<int>::is_always_lock_free,
              "a signal handler may only touch lock-free atomics");

void on_quit_signal(int) { g_quit.store(1, std::memory_order_release); }

}  // namespace

void install_quit_signals() {
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
    // sonora-session.service sets TTYVHangup=yes, and a hangup on tty1 at
    // its default action killed the engine with the take's headers unwritten.
    std::signal(SIGHUP, on_quit_signal);
}

bool quit_requested() { return g_quit.load(std::memory_order_acquire) != 0; }

void clear_quit_request() { g_quit.store(0, std::memory_order_release); }

}  // namespace sonora
