// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control channel and the shared region (ADR-0007 §3, ADR-0008 P9):
// datagram boundaries and order, a region the receiver can read and the
// kernel will not let it write, a peer's close, shutdown() waking a blocked
// reader, a live endpoint refused and a dead one's socket file replaced,
// and child processes.
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "sonora/platform/ipc.h"
#include "sonora/platform/process.h"

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
    a_closed_peer_reads_as_closed();
    shutdown_wakes_a_blocked_reader();
    a_live_endpoint_is_refused_and_a_dead_one_replaced();
    child_processes();
    ::rmdir(("/tmp/sonora-test-ipc-" + std::to_string(::getpid())).c_str());
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("platform ipc: ok\n");
    return 0;
}
