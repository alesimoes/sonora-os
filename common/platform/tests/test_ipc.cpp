// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control channel and the shared region (ADR-0007 §3, ADR-0008 P9):
// datagram boundaries and order, a region the receiver can read and the
// kernel will not let it write, a peer's close, shutdown() waking a blocked
// reader, a live endpoint refused and a dead one's socket file replaced,
// and child processes.
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "sonora/platform/ipc.h"
#include "sonora/platform/process.h"
#include "sonora/platform/wake.h"

#include <atomic>

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

using namespace sonora::platform;

namespace {

std::string temp_endpoint(const char* what) {
    return "/tmp/sonora-test-ipc-" + std::to_string(::getpid()) + "/" + what + ".sock";
}

struct Pair {
    std::unique_ptr<ControlListener> listener;
    std::unique_ptr<ControlChannel> client;
    std::unique_ptr<ControlChannel> server;
};

Pair make_pair(const char* what) {
    Pair p;
    std::string error;
    p.listener = ControlListener::listen(temp_endpoint(what), error);
    CHECK(p.listener != nullptr);
    if (!p.listener) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return p;
    }
    p.client = ControlChannel::connect(temp_endpoint(what), error);
    CHECK(p.client != nullptr);
    p.server = p.listener->accept(1000, error);
    CHECK(p.server != nullptr);
    return p;
}

void datagrams_keep_boundaries_and_order() {
    Pair p = make_pair("order");
    if (!p.server) return;
    std::string error;
    CHECK(p.client->send("one", 0, error) == IoStatus::Ok);
    CHECK(p.client->send(std::string(60000, 'x'), 0, error) == IoStatus::Ok);
    CHECK(p.client->send("three", 0, error) == IoStatus::Ok);
    std::string got;
    CHECK(p.server->receive(got, 70000, nullptr, 1000, error) == IoStatus::Ok && got == "one");
    CHECK(p.server->receive(got, 70000, nullptr, 1000, error) == IoStatus::Ok &&
          got.size() == 60000);
    CHECK(p.server->receive(got, 70000, nullptr, 1000, error) == IoStatus::Ok && got == "three");
    // Nothing waiting: a zero timeout returns at once.
    CHECK(p.server->receive(got, 70000, nullptr, 0, error) == IoStatus::WouldBlock);
}

void an_oversized_datagram_is_an_error_not_a_truncation() {
    Pair p = make_pair("oversize");
    if (!p.server) return;
    std::string error;
    CHECK(p.client->send(std::string(2000, 'y'), 0, error) == IoStatus::Ok);
    std::string got;
    CHECK(p.server->receive(got, 1000, nullptr, 1000, error) == IoStatus::Error);
    CHECK(!error.empty());
}

void the_region_is_read_only_in_the_receiver() {
    Pair p = make_pair("region");
    if (!p.server) return;
    std::string error;
    auto region = SharedRegion::create("sonora-test", 8192, error);
    CHECK(region != nullptr && region->writable());
    if (!region) return;
    static_cast<char*>(region->writable_data())[100] = 42;
    CHECK(p.server->send_with_region("hello", *region, 0, error) == IoStatus::Ok);

    std::string got;
    std::unique_ptr<SharedRegion> seen;
    CHECK(p.client->receive(got, 1000, &seen, 1000, error) == IoStatus::Ok && got == "hello");
    CHECK(seen != nullptr);
    if (!seen) return;
    CHECK(!seen->writable() && seen->writable_data() == nullptr);
    CHECK(seen->bytes() == 8192);
    CHECK(static_cast<const char*>(seen->data())[100] == 42);
    // The creator's later writes are visible: one file, two mappings.
    static_cast<char*>(region->writable_data())[200] = 7;
    CHECK(static_cast<const char*>(seen->data())[200] == 7);
    // The kernel refuses to make the receiver's mapping writable.
    const int rc = ::mprotect(const_cast<void*>(seen->data()), 4096, PROT_READ | PROT_WRITE);
    CHECK(rc != 0 && errno == EACCES);
}

// ADR-0012 Unverified 2: a plugin host sleeps on the clock region's tick
// through a READ-ONLY mapping of a sealed memfd (auditor M1), and the engine
// wakes it through its writable mapping of the same page. FUTEX_WAIT only
// reads the word and a shared futex is keyed on the file, not the mapping,
// so this is expected to work; here it is checked. A wake that did not reach
// the read-only mapping would leave the waiter asleep for its 5 s timeout.
void a_futex_wait_works_on_a_read_only_mapping() {
    Pair p = make_pair("futex");
    if (!p.server) return;
    std::string error;
    auto region = SharedRegion::create("sonora-test-clock", 4096, error);
    CHECK(region != nullptr);
    if (!region) return;
    CHECK(p.server->send_with_region("clock", *region, 0, error) == IoStatus::Ok);
    std::string got;
    std::unique_ptr<SharedRegion> seen;
    CHECK(p.client->receive(got, 1000, &seen, 1000, error) == IoStatus::Ok && seen &&
          !seen->writable());
    if (!seen) return;
    auto* tick = static_cast<std::atomic<uint32_t>*>(region->writable_data());
    const auto* ro_tick = static_cast<const std::atomic<uint32_t>*>(seen->data());

    // Already changed: returns at once.
    tick->store(5, std::memory_order_release);
    const auto t0 = std::chrono::steady_clock::now();
    wait_for_change(*ro_tick, 4, 5'000'000'000ull);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));

    std::atomic<bool> woke{false};
    std::chrono::steady_clock::duration slept{};
    std::thread waiter([&] {
        const auto start = std::chrono::steady_clock::now();
        const uint32_t value = ro_tick->load(std::memory_order_acquire);
        while (ro_tick->load(std::memory_order_acquire) == value &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
            wait_for_change(*ro_tick, value, 5'000'000'000ull);
        slept = std::chrono::steady_clock::now() - start;
        woke.store(true, std::memory_order_release);
    });
    // Long enough for the waiter to be inside FUTEX_WAIT.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tick->fetch_add(1, std::memory_order_release);
    wake_all(*tick);
    waiter.join();
    CHECK(woke.load());
    CHECK(slept < std::chrono::seconds(2));
    std::printf("  futex on a read-only mapping: woken after %.1f ms\n",
                std::chrono::duration<double, std::milli>(slept).count());
}

void a_closed_peer_reads_as_closed() {
    Pair p = make_pair("closed");
    if (!p.server) return;
    p.client.reset();
    std::string got, error;
    CHECK(p.server->receive(got, 1000, nullptr, 1000, error) == IoStatus::Closed);
    CHECK(p.server->send("x", 0, error) == IoStatus::Closed);
}

void shutdown_wakes_a_blocked_reader() {
    Pair p = make_pair("wake");
    if (!p.server) return;
    IoStatus status = IoStatus::Ok;
    std::thread reader([&]() {
        std::string got, error;
        status = p.server->receive(got, 1000, nullptr, -1, error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p.server->shutdown();
    reader.join();
    CHECK(status == IoStatus::Closed);
    // The peer sees it too.
    std::string got, error;
    CHECK(p.client->receive(got, 1000, nullptr, 1000, error) == IoStatus::Closed);
}

void a_live_endpoint_is_refused_and_a_dead_one_replaced() {
    const std::string ep = temp_endpoint("live");
    std::string error;
    auto first = ControlListener::listen(ep, error);
    CHECK(first != nullptr);
    CHECK(endpoint_live(ep));
    auto second = ControlListener::listen(ep, error);
    CHECK(second == nullptr && error.find("already listening") != std::string::npos);
    first.reset();
    CHECK(!endpoint_live(ep));

    // A socket file left by a process that died: bound, never unlinked.
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, ep.c_str(), sizeof addr.sun_path - 1);
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    ::close(fd);
    CHECK(::access(ep.c_str(), F_OK) == 0);
    auto third = ControlListener::listen(ep, error);
    CHECK(third != nullptr);
    third.reset();
    CHECK(::access(ep.c_str(), F_OK) != 0);
}

// The /tmp/sonora-<uid> fallback is in a directory anyone can create in:
// another user who makes it first, or opens it up, could put their own
// socket where the interface connects. A socket directory not ours alone is
// refused.
void a_socket_dir_not_ours_alone_is_refused() {
    const std::string base = "/tmp/sonora-test-ipc-" + std::to_string(::getpid()) + "-dirs";
    ::mkdir(base.c_str(), 0700);
    std::string error;

    const std::string open_dir = base + "/open";
    CHECK(::mkdir(open_dir.c_str(), 0700) == 0);
    ::chmod(open_dir.c_str(), 0755);
    CHECK(ControlListener::listen(open_dir + "/engine.sock", error) == nullptr);
    CHECK(error.find(open_dir) != std::string::npos);
    CHECK(::access((open_dir + "/engine.sock").c_str(), F_OK) != 0);

    // Not a directory of ours: /tmp is root's, or open to all when we are root.
    error.clear();
    CHECK(ControlListener::listen("/tmp/sonora-test-ipc-shared.sock", error) == nullptr);
    CHECK(!error.empty());

    // A symlink to a directory of ours is still refused: what it points to
    // can be changed by whoever owns the link's directory.
    const std::string real = base + "/real";
    const std::string link = base + "/link";
    CHECK(::mkdir(real.c_str(), 0700) == 0);
    CHECK(::symlink(real.c_str(), link.c_str()) == 0);
    error.clear();
    CHECK(ControlListener::listen(link + "/engine.sock", error) == nullptr);
    CHECK(!error.empty());

    // Ours alone: accepted, created or not.
    error.clear();
    auto ok = ControlListener::listen(real + "/engine.sock", error);
    CHECK(ok != nullptr);
    ok.reset();
    auto made = ControlListener::listen(base + "/new/engine.sock", error);
    CHECK(made != nullptr);
    made.reset();

    ::unlink(link.c_str());
    ::rmdir(real.c_str());
    ::rmdir((base + "/new").c_str());
    ::rmdir(open_dir.c_str());
    ::rmdir(base.c_str());
}

// SO_PEERCRED: the engine accepts only its own user, and an interface talks
// only to an engine of its own user. Needs root to be someone else.
void another_user_is_refused_both_ways() {
    if (::geteuid() != 0) {
        std::printf("SKIP another_user_is_refused_both_ways: needs root to change uid\n");
        return;
    }
    const std::string dir = "/tmp/sonora-test-ipc-" + std::to_string(::getpid()) + "-peer";
    const std::string ep = dir + "/engine.sock";
    std::string error;
    auto listener = ControlListener::listen(ep, error);
    CHECK(listener != nullptr);
    if (!listener) return;
    // Opened up after the check, as a misconfiguration would, so that only
    // the credential check stands between the other user and the engine.
    ::chmod(dir.c_str(), 0711);
    ::chmod(ep.c_str(), 0666);

    const pid_t child = ::fork();
    if (child == 0) {
        if (::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(2);
        std::string e;
        auto c = ControlChannel::connect(ep, e);
        // Refused at once: we are not the listener's user.
        if (c != nullptr) ::_exit(3);
        // A raw connection, to see the engine side refuse it too.
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, ep.c_str(), sizeof addr.sun_path - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) ::_exit(4);
        char b[16];
        // The engine closes it: end of file, never a Hello.
        const ssize_t n = ::recv(fd, b, sizeof b, 0);
        ::_exit(n <= 0 ? 0 : 5);
    }
    // Both of the child's connections: the one it gave up on and the raw one.
    for (int i = 0; i < 2; ++i) {
        error.clear();
        auto accepted = listener->accept(5000, error);
        CHECK(accepted == nullptr);
        CHECK(error.find("uid 65534") != std::string::npos);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // A listener of another user, as a hijacked /tmp/sonora-<uid> would hold.
    listener.reset();
    const std::string other_dir = dir + "-other";
    CHECK(::mkdir(other_dir.c_str(), 0777) == 0);
    ::chmod(other_dir.c_str(), 0777);
    const std::string other_ep = other_dir + "/engine.sock";
    int ready[2];
    CHECK(::pipe(ready) == 0);
    const pid_t other = ::fork();
    if (other == 0) {
        ::close(ready[0]);
        if (::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(2);
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, other_ep.c_str(), sizeof addr.sun_path - 1);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) ::_exit(3);
        ::chmod(other_ep.c_str(), 0666);
        if (::listen(fd, 4) != 0) ::_exit(4);
        (void)!::write(ready[1], "r", 1);
        const int c = ::accept(fd, nullptr, nullptr);
        ::sleep(2);
        ::close(c);
        ::_exit(0);
    }
    ::close(ready[1]);
    char r = 0;
    CHECK(::read(ready[0], &r, 1) == 1);
    ::close(ready[0]);
    error.clear();
    CHECK(ControlChannel::connect(other_ep, error) == nullptr);
    CHECK(error.find("uid 65534") != std::string::npos);
    ::kill(other, SIGKILL);
    ::waitpid(other, &status, 0);
    ::unlink(other_ep.c_str());
    ::rmdir(other_dir.c_str());
    ::rmdir(dir.c_str());
}

void child_processes() {
    std::string error;
    auto ok = ChildProcess::spawn({"/bin/sh", "-c", "exit 3"}, {}, error);
    CHECK(ok != nullptr);
    if (ok) {
        CHECK(ok->wait(5000));
        CHECK(ok->exited_normally() && ok->exit_code() == 3);
    }
    auto sleeper = ChildProcess::spawn({"/bin/sh", "-c", "sleep 30"}, {}, error);
    CHECK(sleeper != nullptr);
    if (sleeper) {
        CHECK(sleeper->running());
        sleeper->kill();
        CHECK(sleeper->wait(5000));
        CHECK(!sleeper->exited_normally() && sleeper->signal_number() == 9);
    }
    auto missing = ChildProcess::spawn({"/nonexistent/sonora"}, {}, error);
    // posix_spawn may report a missing program as exit 127 instead.
    if (missing) {
        CHECK(missing->wait(5000));
        CHECK(missing->exited_normally() && missing->exit_code() == 127);
    } else {
        CHECK(!error.empty());
    }
    CHECK(!executable_dir().empty());
}

}  // namespace

int main() {
    CHECK(ipc_supported());
    CHECK(!default_control_endpoint().empty());
    datagrams_keep_boundaries_and_order();
    an_oversized_datagram_is_an_error_not_a_truncation();
    the_region_is_read_only_in_the_receiver();
    a_futex_wait_works_on_a_read_only_mapping();
    a_closed_peer_reads_as_closed();
    shutdown_wakes_a_blocked_reader();
    a_live_endpoint_is_refused_and_a_dead_one_replaced();
    a_socket_dir_not_ours_alone_is_refused();
    another_user_is_refused_both_ways();
    child_processes();
    ::rmdir(("/tmp/sonora-test-ipc-" + std::to_string(::getpid())).c_str());
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("platform ipc: ok\n");
    return 0;
}
