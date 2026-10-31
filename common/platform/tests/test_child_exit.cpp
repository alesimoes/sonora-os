// SPDX-License-Identifier: GPL-3.0-or-later
//
// ChildProcess::exit_handle(), the pidfd a plugin host's owner sleeps on
// instead of polling waitpid (ADR-0012 §4): readable when the child exits
// and not before, kept by no other child, and what wait() sleeps on.
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "sonora/platform/process.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

using sonora::platform::ChildProcess;

namespace {

int readable(int fd, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN) != 0;
}

// A child that has not exited leaves the handle quiet; a kill makes it
// readable, and running() then agrees without waiting.
int test_readable_on_exit() {
    std::string error;
    auto c = ChildProcess::spawn({"/bin/sleep", "30"}, {}, error);
    CHECK(c != nullptr);
    const int fd = c->exit_handle();
    CHECK(fd >= 3);
    CHECK(!readable(fd, 100));
    CHECK(c->running());
    c->kill();
    CHECK(readable(fd, 5000));
    CHECK(!c->running());
    CHECK(c->signal_number() == 9);
    // Still readable, and the same handle, after the reap.
    CHECK(c->exit_handle() == fd);
    CHECK(readable(fd, 0));
    return 0;
}

// A child that exits on its own, before anyone looks: a zombie's handle
// is readable at once, and wait() reaps it with its status.
int test_exit_before_wait() {
    std::string error;
    auto c = ChildProcess::spawn({"/bin/sh", "-c", "exit 7"}, {}, error);
    CHECK(c != nullptr);
    CHECK(c->exit_handle() >= 3);
    CHECK(readable(c->exit_handle(), 5000));
    CHECK(c->wait(0));
    CHECK(c->exited_normally() && c->exit_code() == 7);
    return 0;
}

// wait() with a bound sleeps on the handle: it times out on a live child,
// and returns once one exits well before its bound.
int test_wait_sleeps_on_handle() {
    std::string error;
    auto c = ChildProcess::spawn({"/bin/sleep", "0.2"}, {}, error);
    CHECK(c != nullptr);
    CHECK(!c->wait(20));
    const auto start = std::chrono::steady_clock::now();
    CHECK(c->wait(10000));
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took < std::chrono::seconds(5));
    CHECK(c->exited_normally() && c->exit_code() == 0);
    return 0;
}

// Close-on-exec: a second child, the next plugin host, never holds the
// first one's handle (it would keep nothing alive, but a leaked descriptor
// in a plugin host is one more thing third-party code can touch).
int test_not_inherited() {
    std::string error;
    auto first = ChildProcess::spawn({"/bin/sleep", "30"}, {}, error);
    CHECK(first != nullptr);
    const int fd = first->exit_handle();
    CHECK(fd >= 3);
    auto second = ChildProcess::spawn(
        {"/bin/sh", "-c", "test ! -e /proc/self/fd/" + std::to_string(fd)}, {}, error);
    CHECK(second != nullptr);
    CHECK(second->wait(5000));
    CHECK(second->exited_normally() && second->exit_code() == 0);
    return 0;
}

// The destructor closes it: the number is free again.
int test_closed_with_owner() {
    std::string error;
    int fd = -1;
    {
        auto c = ChildProcess::spawn({"/bin/sh", "-c", "exit 0"}, {}, error);
        CHECK(c != nullptr);
        fd = c->exit_handle();
        CHECK(fd >= 3);
        CHECK(c->wait(5000));
    }
    CHECK(::fcntl(fd, F_GETFD) == -1);
    return 0;
}

// A handle that polls as something other than POLLIN (here POLLHUP: a pipe
// whose writer is gone, put in the pidfd's place) is not an exit. wait()
// with a bound on a live child must time out, not block in waitpid until the
// child ends; past that, the bound falls back to polling waitpid.
int test_hangup_is_not_an_exit() {
    std::string error;
    auto c = ChildProcess::spawn({"/bin/sleep", "30"}, {}, error);
    CHECK(c != nullptr);
    const int fd = c->exit_handle();
    CHECK(fd >= 3);
    int p[2];
    CHECK(::pipe(p) == 0);
    CHECK(::dup2(p[0], fd) == fd);
    ::close(p[0]);
    ::close(p[1]);
    const auto start = std::chrono::steady_clock::now();
    CHECK(!c->wait(100));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(c->running());
    c->kill();
    CHECK(c->wait(5000));
    CHECK(c->signal_number() == 9);
    return 0;
}

}  // namespace

int main() {
    // A kernel before 5.3, or a seccomp profile that refuses pidfd_open: the
    // handle is -1 by contract, and there is nothing here to test.
    {
        std::string error;
        auto probe = ChildProcess::spawn({"/bin/sh", "-c", "exit 0"}, {}, error);
        if (probe && probe->exit_handle() < 0) {
            std::printf("platform_child_exit: no pidfd on this kernel, skipped\n");
            return 77;
        }
    }
    int failed = 0;
    failed += test_hangup_is_not_an_exit();
    failed += test_readable_on_exit();
    failed += test_exit_before_wait();
    failed += test_wait_sleeps_on_handle();
    failed += test_not_inherited();
    failed += test_closed_with_owner();
    if (failed == 0) std::printf("platform_child_exit: all passed\n");
    return failed == 0 ? 0 : 1;
}
