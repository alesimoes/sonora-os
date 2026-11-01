// SPDX-License-Identifier: GPL-3.0-or-later
//
// WaitSet, the control thread's sleep (ADR-0012 §4, M3 on #78): ended by a
// notify() from any thread (none lost, many folded into one), by a child's
// exit handle, by a socket that can take a send again or whose peer hung
// up, and by nothing else before its timeout.
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "sonora/platform/process.h"
#include "sonora/platform/wait_set.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

using sonora::platform::ChildProcess;
using sonora::platform::WaitSet;
using Woke = WaitSet::Woke;
using When = WaitSet::When;

namespace {

long long ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// A notify() before the wait ends it at once; three fold into that one; the
// next wait sleeps its timeout out.
int test_notify_before() {
    WaitSet w;
    w.notify();
    w.notify();
    w.notify();
    CHECK(w.wait(nullptr, 0, 5000) == Woke::Notified);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(w.wait(nullptr, 0, 60) == Woke::Timeout);
    CHECK(ms_since(t0) >= 50);
    return 0;
}

// From another thread, during a wait with no timeout.
int test_notify_across() {
    WaitSet w;
    std::thread t([&w]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        w.notify();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const Woke r = w.wait(nullptr, 0, -1);
    t.join();
    CHECK(r == Woke::Notified);
    CHECK(ms_since(t0) < 2000);
    return 0;
}

// A negative handle is no handle: the wait is the timeout's.
int test_negative_ignored() {
    WaitSet w;
    const WaitSet::Watch none[2] = {{-1, When::Readable}, {-1, When::Writable}};
    CHECK(w.wait(none, 2, 40) == Woke::Timeout);
    return 0;
}

// A child's exit handle: quiet while it runs, a wake once it is killed.
int test_exit_handle() {
    std::string error;
    auto c = ChildProcess::spawn({"/bin/sleep", "30"}, {}, error);
    CHECK(c != nullptr);
    if (c->exit_handle() < 0) {
        std::printf("  no pidfd here: exit_handle skipped\n");
        return 0;
    }
    WaitSet w;
    const WaitSet::Watch exit{c->exit_handle(), When::Readable};
    CHECK(w.wait(&exit, 1, 100) == Woke::Timeout);
    c->kill();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(w.wait(&exit, 1, 5000) == Woke::Handle);
    CHECK(ms_since(t0) < 2000);
    CHECK(!c->running());
    return 0;
}

// Writable is level: ready at once on an empty socket, not while the peer's
// queue is full, ready again once the peer reads, and a hang-up ends it.
int test_writable() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(sv[1], F_SETFL, ::fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    WaitSet w;
    const WaitSet::Watch out{sv[0], When::Writable};
    CHECK(w.wait(&out, 1, 1000) == Woke::Handle);

    const std::string datagram(1024, 'x');
    int sent = 0;
    while (::send(sv[0], datagram.data(), datagram.size(), MSG_NOSIGNAL) > 0) ++sent;
    CHECK(sent > 0);
    CHECK(w.wait(&out, 1, 100) == Woke::Timeout);
    // A Readable watch on the reader's side sees what waits.
    const WaitSet::Watch in{sv[1], When::Readable};
    CHECK(w.wait(&in, 1, 1000) == Woke::Handle);

    char buf[2048];
    while (::recv(sv[1], buf, sizeof buf, 0) > 0) {
    }
    CHECK(w.wait(&out, 1, 1000) == Woke::Handle);

    while (::send(sv[0], datagram.data(), datagram.size(), MSG_NOSIGNAL) > 0) {
    }
    CHECK(w.wait(&out, 1, 100) == Woke::Timeout);
    ::close(sv[1]);
    CHECK(w.wait(&out, 1, 1000) == Woke::Handle);
    ::close(sv[0]);
    return 0;
}

// A notify and a ready handle together: the notify is said, and consumed.
int test_notify_and_handle() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    WaitSet w;
    const WaitSet::Watch out{sv[0], When::Writable};
    w.notify();
    CHECK(w.wait(&out, 1, 1000) == Woke::Notified);
    CHECK(w.wait(&out, 1, 1000) == Woke::Handle);
    CHECK(w.wait(nullptr, 0, 30) == Woke::Timeout);
    ::close(sv[0]);
    ::close(sv[1]);
    return 0;
}

}  // namespace

int main() {
    WaitSet probe;
    if (!probe.watches_handles()) {
        std::fprintf(stderr, "FAIL: no self-pipe, so no handle can be watched\n");
        return 1;
    }
    int failures = 0;
    failures += test_notify_before();
    failures += test_notify_across();
    failures += test_negative_ignored();
    failures += test_exit_handle();
    failures += test_writable();
    failures += test_notify_and_handle();
    if (failures == 0) std::printf("platform_wait_set: ok\n");
    return failures == 0 ? 0 : 1;
}
