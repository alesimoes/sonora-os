// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every signal that ends the engine ends it through session.stop().
//
// sonora-session.service sets TTYVHangup=yes, and SIGHUP had no handler, so a
// hangup on tty1 killed the engine outright: no stop(), and every WAV header
// of the take in progress left with zero sizes.
#include <csignal>
#include <cstdio>
#include <initializer_list>

#include "sonora/quit_signals.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    sonora::install_quit_signals();
    for (const int sig : {SIGHUP, SIGTERM, SIGINT}) {
        sonora::clear_quit_request();
        CHECK(!sonora::quit_requested());
        // Without a handler, SIGHUP and SIGTERM terminate this process here
        // and ctest reports the test as failed.
        CHECK(std::raise(sig) == 0);
        CHECK(sonora::quit_requested());
    }
    std::printf("test_quit_signals: ok\n");
    return 0;
}
