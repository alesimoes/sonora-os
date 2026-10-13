// SPDX-License-Identifier: GPL-3.0-or-later
//
// After a stall the take is closed, but leaving waits in jack_deactivate()
// and jack_client_close() for a server that may never answer, and
// Restart=always cannot act on a process that never exits. The watchdog
// bounds that wait: past its deadline the process ends with a message.
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "sonora/exit_watchdog.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

// Waits up to `limit_s` for the child; kills it and reports -1 if it is
// still running then.
int wait_child(pid_t pid, int limit_s, double& took_s) {
    const auto start = std::chrono::steady_clock::now();
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        took_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                     .count();
        if (r == pid) return status;
        if (took_s > limit_s) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -1;
        }
        ::usleep(20000);
    }
}

// A teardown that hangs: the child blocks for good after arming.
int test_hung_teardown_is_ended() {
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        sonora::arm_exit_watchdog(1, "test: teardown hung\n");
        for (;;) ::pause();
    }
    double took = 0.0;
    const int status = wait_child(pid, 10, took);
    CHECK(status != -1);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(took >= 0.9);
    return 0;
}

// A teardown that finishes in time keeps its own exit status.
int test_prompt_exit_keeps_its_status() {
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        sonora::arm_exit_watchdog(5, "test: should not print\n");
        std::exit(0);
    }
    double took = 0.0;
    const int status = wait_child(pid, 10, took);
    CHECK(status != -1);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(took < 4.0);
    return 0;
}

// Armed twice -- by the explicit call and by the guard on the way out --
// it still fires once, at the first deadline.
int test_arming_twice_keeps_the_first_deadline() {
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        sonora::arm_exit_watchdog(1, "test: first\n");
        sonora::arm_exit_watchdog(30, "test: second\n");
        for (;;) ::pause();
    }
    double took = 0.0;
    const int status = wait_child(pid, 10, took);
    CHECK(status != -1);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    CHECK(took < 5.0);
    return 0;
}

}  // namespace

int main() {
    if (test_hung_teardown_is_ended() != 0) return 1;
    if (test_prompt_exit_keeps_its_status() != 0) return 1;
    if (test_arming_twice_keeps_the_first_deadline() != 0) return 1;
    std::printf("test_exit_watchdog: ok\n");
    return 0;
}
