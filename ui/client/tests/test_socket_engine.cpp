// SPDX-License-Identifier: GPL-3.0-or-later
//
// SocketEngine against sonora-engine running as a process of its own
// (ADR-0007 §3, M7), on drivers/fake (--backend fake), across a real
// socket and a real memfd:
//
// - the control API's conformance suite, its third implementation after
//   FakeEngine and InProcessEngine; the engine is killed with SIGKILL for
//   the heartbeat check, as a crash would;
// - an interface process killed mid-take: the engine records on, and the
//   files are valid when a second interface stops the take;
// - a client that reconnects is resynced, and its undo and redo stay in step
//   with the engine's and with another client's; after the engine itself
//   restarts, the client reattaches to the new state block;
// - malformed frames from a client cost that client its connection, and
//   nothing else.
//
// With --record-child ENDPOINT this binary is the interface that gets
// killed: it arms, records, says so, and waits to die.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/conformance.h"
#include "sonora/api/wire.h"
#include "sonora/client/socket_engine.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
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

namespace {

namespace fs = std::filesystem;
using namespace sonora;
using Clock = std::chrono::steady_clock;
using client::SocketEngine;

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now().time_since_epoch())
                                     .count());
}

fs::path fresh_dir(const char* what) {
    static int n = 0;
    fs::path p = fs::temp_directory_path() /
                 ("sonora_test_socket_" + std::to_string(::getpid()) + "_" + what + "_" +
                  std::to_string(++n));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

// sonora-engine as a process: headless, on drivers/fake, four tracks, its
// socket and session in `dir`.
class EngineProcess {
public:
    EngineProcess(const fs::path& dir, uint32_t protocol = api::kProtocolVersion,
                  uint32_t layout = api::kStateLayoutVersion)
        : dir_(dir), endpoint_((dir / "sock" / "engine.sock").string()) {
        start(protocol, layout);
    }
    ~EngineProcess() { stop(); }

    void start(uint32_t protocol = api::kProtocolVersion,
               uint32_t layout = api::kStateLayoutVersion) {
        platform::ChildProcess::Options o;
        o.stdout_path = (dir_ / ("engine" + std::to_string(++runs_) + ".out")).string();
        o.stderr_path = (dir_ / ("engine" + std::to_string(runs_) + ".err")).string();
        std::string error;
        process_ = platform::ChildProcess::spawn(
            {SONORA_ENGINE_BIN, "--backend", "fake", "--tracks", "4", "--dir",
             (dir_ / "session").string(), "--control-socket", endpoint_, "--headless",
             "--no-memlock", "--announce-versions", std::to_string(protocol),
             std::to_string(layout)},
            o, error);
        if (!process_) std::fprintf(stderr, "  engine: %s\n", error.c_str());
    }

    // Asks it to leave and returns its exit status, or -1.
    int stop() {
        if (!process_) return -1;
        process_->terminate();
        if (!process_->wait(10000)) process_->kill();
        process_->wait(-1);
        const int rc = process_->exited_normally() ? process_->exit_code() : -1;
        process_.reset();
        return rc;
    }

    void kill() {
        if (!process_) return;
        process_->kill();
        process_->wait(-1);
        process_.reset();
    }

    const std::string& endpoint() const { return endpoint_; }
    fs::path session() const { return dir_ / "session"; }
    bool running() { return process_ && process_->running(); }

private:
    fs::path dir_;
    std::string endpoint_;
    int runs_ = 0;
    std::unique_ptr<platform::ChildProcess> process_;
};

SocketEngine::Options client_options(const std::string& endpoint) {
    SocketEngine::Options o;
    o.endpoint = endpoint;
    // A sanitizer build starts slowly.
    o.connect_timeout_ms = 20000;
    o.reconnect_period_ns = 50'000'000;
    return o;
}

// The engine process and a SocketEngine on it, as the suite sees one engine.
class Remote final : public api::EngineControl {
public:
    Remote(uint32_t protocol, uint32_t layout)
        : dir_(fresh_dir("conformance")),
          engine_(dir_, protocol, layout),
          client_(client_options(engine_.endpoint())) {}
    ~Remote() override {
        engine_.stop();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    bool connect(std::string& e) override { return client_.connect(e); }
    const api::Hello& hello() const override { return client_.hello(); }
    uint64_t send(const api::CommandRequest& r) override { return client_.send(r); }
    uint64_t send(const api::ParameterChange& c) override { return client_.send(c); }
    uint64_t submit(const api::ModelRequest& m) override { return client_.submit(m); }
    void poll(api::Inbox& in) override { client_.poll(in); }
    api::StateView& state() override { return client_.state(); }

    void kill() { engine_.kill(); }

private:
    fs::path dir_;
    EngineProcess engine_;
    SocketEngine client_;
};

class SocketRig final : public api::conformance::Rig {
public:
    const char* name() const override { return "SocketEngine"; }
    std::unique_ptr<api::EngineControl> make(uint32_t protocol, uint32_t layout) override {
        return std::make_unique<Remote>(protocol, layout);
    }
    uint64_t now_ns() override { return steady_ns(); }
    void wait_ns(uint64_t ns) override {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
    void kill(api::EngineControl& e) override { static_cast<Remote&>(e).kill(); }
    bool has_signal() const override { return true; }
};

// Polls `e` until `pred` holds or `ms` pass.
template <typename Pred>
bool until(api::EngineControl& e, api::Inbox& in, Pred pred, int ms = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        e.poll(in);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    e.poll(in);
    return pred();
}

bool command(api::EngineControl& e, const std::string& id, int32_t target = -1) {
    api::CommandRequest r;
    r.command = id;
    r.target = target;
    const uint64_t seq = e.send(r);
    api::Inbox in;
    bool ok = false;
    until(e, in, [&]() {
        for (const auto& rep : in.replies) {
            if (rep.seq == seq) {
                ok = rep.result == DispatchResult::Ok;
                if (!ok) std::fprintf(stderr, "  %s: %s\n", id.c_str(), rep.message.c_str());
                return true;
            }
        }
        return false;
    });
    return ok;
}

bool wav_valid(const fs::path& p, uint64_t& data_bytes) {
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
    auto u32 = [&b](std::size_t at) {
        return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8 |
               static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
    };
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    if (u32(4) != b.size() - 8) return false;
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        const uint32_t size = u32(at + 4);
        if (std::memcmp(&b[at], "data", 4) == 0) {
            data_bytes = size;
            return at + 8 + size == b.size() && size > 0 && size % 3 == 0;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

// --record-child: the interface that is killed.
int record_child(const std::string& endpoint) {
    SocketEngine e(client_options(endpoint));
    std::string error;
    if (!e.connect(error)) {
        std::fprintf(stderr, "child: %s\n", error.c_str());
        return 1;
    }
    if (!command(e, "track.arm_all") || !command(e, "transport.record")) return 1;
    std::printf("child: recording\n");
    std::fflush(stdout);
    for (;;) {
        api::Inbox in;
        e.poll(in);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void an_interface_killed_mid_take(const char* self) {
    const fs::path dir = fresh_dir("killed");
    EngineProcess engine(dir);
    SocketEngine watcher(client_options(engine.endpoint()));
    std::string error;
    CHECK(watcher.connect(error));
    if (!watcher.connected()) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return;
    }
    platform::ChildProcess::Options o;
    o.stdout_path = (dir / "child.out").string();
    o.stderr_path = (dir / "child.err").string();
    auto ui = platform::ChildProcess::spawn({self, "--record-child", engine.endpoint()}, o, error);
    CHECK(ui != nullptr);
    if (!ui) return;
    api::Inbox in;
    auto& v = watcher.state();
    CHECK(until(watcher, in,
                [&]() { return v.transport().state == api::TransportState::Recording; }, 20000));
    // As a crash: no goodbye on the socket.
    ui->kill();
    CHECK(ui->wait(5000) && !ui->exited_normally());
    const uint64_t at = v.transport().playhead;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(until(watcher, in, [&]() {
        return v.transport().state == api::TransportState::Recording &&
               v.transport().playhead > at + 48000 / 5;
    }));
    CHECK(v.engine_alive(steady_ns()));
    CHECK(engine.running());

    // The interface that is left stops the take, and it is whole.
    CHECK(command(watcher, "transport.stop"));
    CHECK(until(watcher, in,
                [&]() { return v.transport().state == api::TransportState::Stopped; }));
    CHECK(v.transport().overruns == 0 && v.transport().xruns == 0);
    int files = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(engine.session(), ec)) {
        if (entry.path().extension() != ".wav") continue;
        uint64_t bytes = 0;
        CHECK(wav_valid(entry.path(), bytes));
        // Half a second at 48 kHz, 24-bit, at the least: it went on past the kill.
        CHECK(bytes >= 3ull * 48000 / 2);
        ++files;
    }
    CHECK(files == 4);
    CHECK(engine.stop() == 0);
    fs::remove_all(dir, ec);
}

// A replica fed from an inbox, as an interface's tick feeds it.
struct Replicated {
    explicit Replicated(const std::string& endpoint) : engine(client_options(endpoint)) {}

    void pump() {
        engine.poll(in);
        for (const api::ModelUpdate& m : in.model) {
            model::ReplicationUpdate u;
            u.kind = m.kind == api::ModelUpdateKind::Op     ? model::ReplicationUpdate::Kind::Op
                     : m.kind == api::ModelUpdateKind::Undo ? model::ReplicationUpdate::Kind::Undo
                     : m.kind == api::ModelUpdateKind::Redo ? model::ReplicationUpdate::Kind::Redo
                                                            : model::ReplicationUpdate::Kind::Snapshot;
            u.revision = m.revision;
            u.undo_floor = m.undo_floor;
            u.next_id = m.next_id;
            u.bytes = m.bytes;
            std::string why;
            if (!replica.receive(u, why)) error = why;
        }
        in.clear();
    }
    uint64_t revision() const {
        return replica.synced() ? replica.current().revision : ~uint64_t{0};
    }
    std::size_t markers() const {
        return replica.synced() ? replica.current().edit->markers->markers.size() : 9999;
    }
    bool submit(api::ModelAction action, const std::string& op = {}) {
        api::ModelRequest m;
        m.action = action;
        m.op = op;
        engine.submit(m);
        return true;
    }
    bool add_marker(uint64_t position) {
        model::AddMarker add;
        add.marker.id = replica.current().next_id;
        add.marker.position = position;
        add.marker.name = "M";
        std::string bytes, why;
        if (!model::encode_operation(add, bytes, why)) return false;
        return submit(api::ModelAction::Apply, bytes);
    }

    SocketEngine engine;
    model::Replica replica;
    api::Inbox in;
    std::string error;
};

template <typename Pred>
bool settle(Replicated& a, Replicated& b, Pred pred, int ms = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        a.pump();
        b.pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void a_reconnect_resyncs_and_undo_stays_in_step() {
    const fs::path dir = fresh_dir("reconnect");
    EngineProcess engine(dir);
    Replicated a(engine.endpoint()), b(engine.endpoint());
    std::string error;
    CHECK(a.engine.connect(error) && b.engine.connect(error));
    CHECK(settle(a, b, [&]() { return a.replica.synced() && b.replica.synced(); }));
    if (!a.replica.synced() || !b.replica.synced()) return;
    const uint64_t rev0 = a.revision();

    for (uint64_t i = 1; i <= 3; ++i) {
        CHECK(a.add_marker(48000 * i));
        CHECK(settle(a, b, [&]() { return a.revision() == rev0 + i && b.revision() == rev0 + i; }));
    }

    // A loses its connection; B goes on editing without it.
    a.engine.drop_connection("test: the socket went away");
    CHECK(!a.engine.connected());
    CHECK(b.add_marker(48000 * 9));
    CHECK(b.submit(api::ModelAction::Undo));
    CHECK(b.submit(api::ModelAction::Undo));
    CHECK(settle(a, b, [&]() { return b.revision() == rev0 + 6; }));

    // A's next polls reconnect it, and the Snapshot puts its replica level
    // with the engine, undo window and all.
    CHECK(settle(a, b, [&]() {
        return a.engine.connected() && a.engine.greetings() == 2 && a.revision() == rev0 + 6;
    }));
    CHECK(a.error.empty() && b.error.empty());
    CHECK(a.markers() == 2 && b.markers() == 2);
    CHECK(a.replica.can_undo() && a.replica.can_redo());

    // Undo and redo from A apply on both replicas, as on the engine.
    CHECK(a.submit(api::ModelAction::Redo));
    CHECK(settle(a, b, [&]() { return a.revision() == rev0 + 7 && b.revision() == rev0 + 7; }));
    CHECK(a.markers() == 3 && b.markers() == 3);
    CHECK(a.submit(api::ModelAction::Undo));
    CHECK(a.submit(api::ModelAction::Undo));
    CHECK(settle(a, b, [&]() { return a.revision() == rev0 + 9 && b.revision() == rev0 + 9; }));
    CHECK(a.markers() == 1 && b.markers() == 1);
    CHECK(a.error.empty() && b.error.empty());

    // The engine itself restarts: a new process, a new state block. A
    // reattaches to it and is alive again; until then it read "no data".
    api::Inbox in;
    engine.kill();
    CHECK(until(a.engine, in, [&]() { return !a.engine.state().engine_alive(steady_ns()); }));
    engine.start();
    CHECK(until(a.engine, in, [&]() {
        return a.engine.connected() && a.engine.state().engine_alive(steady_ns());
    }, 20000));
    CHECK(a.engine.greetings() >= 3);
    CHECK(engine.stop() == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void malformed_frames_cost_only_their_sender() {
    const fs::path dir = fresh_dir("malformed");
    EngineProcess engine(dir);
    SocketEngine good(client_options(engine.endpoint()));
    std::string error;
    CHECK(good.connect(error));
    CHECK(command(good, "track.arm_all"));
    CHECK(command(good, "transport.record"));

    const auto refused = [&](const std::string& datagram) {
        auto ch = platform::ControlChannel::connect(engine.endpoint(), error);
        if (!ch) return false;
        std::string got;
        // Hello and the rest first, so the refusal is what closes it.
        ch->send(datagram, 1000, error);
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end) {
            const auto s = ch->receive(got, api::wire::kMaxFrameBytes, nullptr, 100, error);
            if (s == platform::IoStatus::Closed || s == platform::IoStatus::Error) return true;
        }
        return false;
    };
    std::string huge(12, '\0');
    const uint32_t gib = 1u << 30;
    std::memcpy(&huge[0], &gib, 4);
    CHECK(refused(huge));
    CHECK(refused(std::string(3, '\x01')));
    CHECK(refused(std::string(api::wire::kMaxFrameBytes + 64, 'x')));

    api::Inbox in;
    auto& v = good.state();
    const uint64_t at = v.transport().playhead;
    CHECK(until(good, in, [&]() {
        return v.transport().state == api::TransportState::Recording &&
               v.transport().playhead > at + 48000 / 10;
    }));
    CHECK(command(good, "transport.stop"));
    CHECK(good.connected() && good.greetings() == 1);
    CHECK(engine.stop() == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--record-child") == 0) return record_child(argv[2]);

    SocketRig rig;
    const int failed = api::conformance::run(rig);
    if (failed != 0) {
        std::fprintf(stderr, "%d conformance check(s) failed on SocketEngine\n", failed);
        failures += failed;
    }
    an_interface_killed_mid_take(argv[0]);
    a_reconnect_resyncs_and_undo_stays_in_step();
    malformed_frames_cost_only_their_sender();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("socket engine: ok\n");
    return 0;
}
