// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplicaDocument across a lost connection, on sonora-engine run as a
// process of its own (ADR-0007 M7). An edit whose request never reached the
// engine, or whose answer never came back, must not be replayed on the
// Snapshot that resyncs the interface: that would show an edit the engine
// lacks, make Undo undo the engine's previous step instead, and make Save
// write a document the engine does not have. Such a request is dropped with
// a notice, and the document shown is the engine's.
//
// An observer interface, never disconnected, says what the engine's
// document is.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "control_adapter.h"
#include "replica_document.h"
#include "sonora/client/socket_engine.h"
#include "sonora/model/session_file.h"
#include "sonora/platform/process.h"
#include "timeline_document.h"

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
using namespace sonora::ui;
using Clock = std::chrono::steady_clock;

class EngineProcess {
public:
    explicit EngineProcess(const fs::path& dir)
        : dir_(dir), endpoint_((dir / "sock" / "engine.sock").string()) {
        platform::ChildProcess::Options o;
        o.stdout_path = (dir_ / "engine.out").string();
        o.stderr_path = (dir_ / "engine.err").string();
        std::string error;
        process_ = platform::ChildProcess::spawn(
            {SONORA_ENGINE_BIN, "--backend", "fake", "--tracks", "4", "--dir",
             (dir_ / "session").string(), "--control-socket", endpoint_, "--headless",
             "--no-memlock"},
            o, error);
        if (!process_) std::fprintf(stderr, "  engine: %s\n", error.c_str());
    }
    // Its stderr is a file deleted with the directory, so it is read here:
    // an RTSan report in it (the rtsan CI job halts the engine on one), or an
    // exit other than 0 when asked to leave, fails the test, printed.
    ~EngineProcess() {
        if (!process_) return;
        process_->terminate();
        if (!process_->wait(10000)) process_->kill();
        process_->wait(-1);
        const bool exit_failed = !process_->exited_normally() || process_->exit_code() != 0;
        std::ifstream f(dir_ / "engine.err");
        const std::string err((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (exit_failed || err.find("ERROR: RealtimeSanitizer") != std::string::npos) {
            std::fprintf(stderr, "FAIL sonora-engine: %s; its stderr:\n%s\n",
                         exit_failed ? "did not exit 0 when asked to stop" : "RealtimeSanitizer report",
                         err.c_str());
            ++failures;
        }
    }
    const std::string& endpoint() const { return endpoint_; }

private:
    fs::path dir_;
    std::string endpoint_;
    std::unique_ptr<platform::ChildProcess> process_;
};

struct Ui {
    explicit Ui(const std::string& endpoint) : se(options(endpoint)) {
        doc.on_refused = [this](const std::string& why) { notices.push_back(why); };
        doc.on_load_abandoned = [this](const std::string&, bool) { ++loads_abandoned; };
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) { doc.receive(u, r); });
        engine.watchConnection([this]() { return se.connected() ? se.greetings() : 0; },
                               [this](bool up) { doc.set_link(up); });
    }
    static client::SocketEngine::Options options(const std::string& endpoint) {
        client::SocketEngine::Options o;
        o.endpoint = endpoint;
        o.connect_timeout_ms = 20000;
        o.reconnect_period_ns = 50'000'000;
        return o;
    }
    void tick() { engine.poll(outcomes); }
    std::size_t markers() const { return doc.snapshot().edit->markers->markers.size(); }

    client::SocketEngine se;
    ControlAdapter engine{se};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    std::vector<CommandOutcome> outcomes;
    std::vector<std::string> notices;
    int loads_abandoned = 0;
};

template <typename Pred>
bool settle(Ui& a, Ui& b, Pred pred, int ms = 10000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        a.tick();
        b.tick();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

model::AddMarker marker(model::Id id, uint64_t position) {
    model::AddMarker m;
    m.marker.id = id;
    m.marker.position = position;
    m.marker.name = "M" + std::to_string(position);
    return m;
}

std::string file_bytes(const model::Snapshot& s) {
    model::WriteInfo info;
    info.written_by = "test";
    std::string bytes, error;
    if (!model::serialize(s, info, bytes, error)) return "unserializable: " + error;
    return bytes;
}

// Settled: A is on a new connection, nothing of its own in flight, and shows
// what the observer shows.
bool level(Ui& a, Ui& o, uint64_t greetings) {
    return settle(a, o, [&]() {
        return a.se.connected() && a.se.greetings() >= greetings && a.doc.synced() &&
               a.doc.in_flight() == 0 && o.doc.in_flight() == 0 &&
               file_bytes(a.doc.snapshot()) == file_bytes(o.doc.snapshot());
    });
}

void a_lost_edit_is_never_replayed(const std::string& endpoint) {
    Ui a(endpoint), o(endpoint);
    std::string error, why;
    CHECK(a.engine.connect(error) && o.engine.connect(error));
    CHECK(settle(a, o, [&]() { return a.doc.synced() && o.doc.synced(); }));

    CHECK(a.doc.submit(marker(a.doc.reserve_id(), 48000), why));
    CHECK(settle(a, o, [&]() { return a.doc.in_flight() == 0 && o.markers() == 1; }));
    CHECK(a.doc.submit(marker(a.doc.reserve_id(), 96000), why));
    CHECK(settle(a, o, [&]() { return a.doc.in_flight() == 0 && o.markers() == 2; }));

    // The connection goes before A's tick has seen it: the edit is accepted
    // on screen, and its request is dropped unsent.
    a.se.drop_connection("test: the socket went away");
    CHECK(a.doc.submit(marker(a.doc.reserve_id(), 144000), why));
    CHECK(a.markers() == 3);

    CHECK(level(a, o, 2));
    // No ghost: the engine never had the third marker, so neither does A.
    CHECK(a.markers() == 2 && o.markers() == 2);
    CHECK(!a.notices.empty());
    CHECK(a.engine.awaiting_model_replies() == 0);

    // Undo is the engine's last step, the second marker, on both.
    CHECK(a.doc.undo(why));
    CHECK(settle(a, o, [&]() { return a.doc.in_flight() == 0 && o.markers() == 1; }));
    CHECK(a.markers() == 1 && o.markers() == 1);
    CHECK(o.doc.snapshot().edit->markers->markers[0].position == 48000);
    // Save writes what the engine has.
    CHECK(file_bytes(a.doc.snapshot()) == file_bytes(o.doc.snapshot()));

    // Once the interface knows the engine is gone it refuses edits outright,
    // and File > Open with them.
    a.se.drop_connection("test: again");
    a.tick();
    CHECK(!a.se.connected());
    why.clear();
    CHECK(!a.doc.submit(marker(a.doc.reserve_id(), 192000), why) && !why.empty());
    CHECK(!a.doc.undo(why));
    CHECK(!a.doc.reset(make_demo_session(4, 48000), why) && !why.empty());
    CHECK(a.markers() == 1);
    CHECK(level(a, o, 3));
    CHECK(a.markers() == 1);

    // Sent, and the connection lost before the answer: the engine may or may
    // not have it; either way A ends up showing the engine's document.
    CHECK(a.doc.submit(marker(a.doc.reserve_id(), 240000), why));
    a.se.drop_connection("test: lost in flight");
    CHECK(level(a, o, 4));
    CHECK(a.markers() == o.markers());

    // File > Open that never reached the engine: the window must stop
    // naming the file it did not open.
    a.se.drop_connection("test: open lost");
    CHECK(a.doc.reset(make_demo_session(4, 48000), why));
    CHECK(level(a, o, 5));
    CHECK(a.loads_abandoned == 1);
    CHECK(a.markers() == o.markers());
    CHECK(a.engine.awaiting_model_replies() == 0);

    // Many lost requests do not pile up in the adapter.
    for (int round = 0; round < 3; ++round) {
        a.se.drop_connection("test: flood");
        for (int i = 0; i < 2000; ++i) {
            api::ModelRequest m;
            m.action = api::ModelAction::Resync;
            a.engine.submitModel(m);
        }
        CHECK(a.engine.awaiting_model_replies() <= ControlAdapter::kMaxAwaiting);
        CHECK(level(a, o, static_cast<uint64_t>(6 + round)));
        CHECK(a.engine.awaiting_model_replies() == 0);
    }
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
                         ("sonora_test_replica_reconnect_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        EngineProcess engine(dir);
        a_lost_edit_is_never_replayed(engine.endpoint());
    }
    fs::remove_all(dir, ec);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("replica reconnect: ok\n");
    return 0;
}
