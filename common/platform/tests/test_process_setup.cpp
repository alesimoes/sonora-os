// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts of process_setup.h that hold on every platform: the quit flag
// set by a signal, and the default session root. Standard C only, so the
// MSVC job builds it with the rest of the neutral modules.
#include <csignal>
#include <cstdio>

#include "sonora/platform/process_setup.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    namespace p = sonora::platform;

    CHECK(p::default_session_root("/home/ann") == "/home/ann/Sessions");
    // No home setting: the working directory, never an absolute "/Sessions".
    CHECK(p::default_session_root(nullptr) == "./Sessions");

    p::install_quit_handlers();
    for (const int sig : {SIGINT, SIGTERM}) {
        p::clear_quit_request();
        CHECK(!p::quit_requested());
        CHECK(std::raise(sig) == 0);
        CHECK(p::quit_requested());
    }
    p::clear_quit_request();
    CHECK(!p::quit_requested());

    CHECK(p::locked_kb() >= -1);

    std::printf("test_process_setup: ok\n");
    return 0;
}
