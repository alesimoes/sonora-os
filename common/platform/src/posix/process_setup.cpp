// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/process_setup.h"

#include <sys/mman.h>
#include <sys/resource.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace sonora::platform {
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

bool lock_memory(std::string& why) {
    rlimit lim{};
    if (::getrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
        why = std::string("getrlimit(RLIMIT_MEMLOCK): ") + std::strerror(errno);
        return false;
    }
    if (lim.rlim_cur != RLIM_INFINITY) {
        // An unprivileged process may raise its soft limit to the hard one;
        // root may raise both.
        const rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
        if (::setrlimit(RLIMIT_MEMLOCK, &unlimited) != 0) {
            char text[160];
            std::snprintf(text, sizeof text,
                          "RLIMIT_MEMLOCK is %llu bytes, not unlimited (the"
                          " service sets LimitMEMLOCK=infinity; elsewhere,"
                          " ulimit -l unlimited)",
                          static_cast<unsigned long long>(lim.rlim_cur));
            why = text;
            return false;
        }
    }
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        why = std::string("mlockall: ") + std::strerror(errno);
        return false;
    }
    return true;
}

long locked_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    long kb = -1;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, "VmLck: %ld kB", &kb) == 1) break;
    }
    std::fclose(f);
    return kb;
}

void install_quit_handlers() {
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
    // sonora-session.service sets TTYVHangup=yes, and a hangup on tty1 at
    // its default action killed the engine with the take's headers unwritten.
    std::signal(SIGHUP, on_quit_signal);
}

bool quit_requested() { return g_quit.load(std::memory_order_acquire) != 0; }

void clear_quit_request() { g_quit.store(0, std::memory_order_release); }

std::string default_session_root(const char* home) {
    return std::string(home ? home : ".") + "/Sessions";
}

}  // namespace sonora::platform
