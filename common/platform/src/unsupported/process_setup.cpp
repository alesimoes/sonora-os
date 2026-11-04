// SPDX-License-Identifier: GPL-3.0-or-later
// The neutral build of process_setup.h: standard C only. The quit flag works
// (SIGINT and SIGTERM exist in the C library); memory locking is refused out
// loud rather than claimed (ADR-0008 §6).
#include "sonora/platform/process_setup.h"

#include <atomic>
#include <csignal>
#include <string>

namespace sonora::platform {
namespace {

std::atomic<int> g_quit{0};
static_assert(std::atomic<int>::is_always_lock_free,
              "a signal handler may only touch lock-free atomics");

void on_quit_signal(int) { g_quit.store(1, std::memory_order_release); }

}  // namespace

bool lock_memory(std::string& why) {
    why = "locking memory is not available on this platform yet (ADR-0008 §6)";
    return false;
}

long locked_kb() { return -1; }

void install_quit_handlers() {
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
}

bool quit_requested() { return g_quit.load(std::memory_order_acquire) != 0; }

void clear_quit_request() { g_quit.store(0, std::memory_order_release); }

std::string default_session_root(const char* home) {
    return std::string(home ? home : ".") + "/Sessions";
}

}  // namespace sonora::platform
