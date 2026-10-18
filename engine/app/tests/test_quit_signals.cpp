// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every signal that ends the engine ends it through session.stop().
//
// sonora-session.service sets TTYVHangup=yes, and SIGHUP had no handler, so a
// hangup on tty1 killed the engine outright: no stop(), and every WAV header
// of the take in progress left with zero sizes.
//
// A process-directed signal runs its handler on whichever thread the kernel
// picks, and sonora-engine polls the flag from main while other threads
// (the control socket's accept loop, the disk thread) run. CI's
// ThreadSanitizer job caught SIGTERM handled on the accept-loop thread while
// main read the flag: a volatile sig_atomic_t is safe only against the
// thread the handler interrupts. The second test raises the signal on
// another thread while this one polls; under TSan the old flag is a
// reported race and the test fails.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <initializer_list>
#include <thread>

#include "sonora/quit_signals.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

int test_each_signal_sets_the_flag() {
    for (const int sig : {SIGHUP, SIGTERM, SIGINT}) {
        sonora::clear_quit_request();
        CHECK(!sonora::quit_requested());
        // Without a handler, SIGHUP and SIGTERM terminate this process here
        // and ctest reports the test as failed.
        CHECK(std::raise(sig) == 0);
        CHECK(sonora::quit_requested());
    }
    return 0;
}

int test_signal_handled_on_another_thread() {
    for (const int sig : {SIGHUP, SIGTERM, SIGINT}) {
        sonora::clear_quit_request();
        std::atomic<bool> polling{false};
        // raise() delivers to the calling thread, so the handler runs there,
        // as it ran on the accept loop in the CI report.
        std::thread other([&polling, sig] {
            while (!polling.load(std::memory_order_acquire)) {
            }
            std::raise(sig);
        });
        bool seen = false;
        polling.store(true, std::memory_order_release);
        for (long i = 0; i < 2000000000L && !seen; ++i) seen = sonora::quit_requested();
        other.join();
        CHECK(seen);
    }
    return 0;
}

}  // namespace

int main() {
    sonora::install_quit_signals();
    if (test_each_signal_sets_the_flag()) return 1;
    if (test_signal_handled_on_another_thread()) return 1;
    std::printf("test_quit_signals: ok\n");
    return 0;
}
