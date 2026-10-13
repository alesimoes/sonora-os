// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control API on the real engine (ADR-0007 M4, §7): the conformance
// suite FakeEngine passes, run on InProcessEngine over a real Session fed
// by drivers/fake at real-time pace; FakeEngine's command list held to the
// handlers this build installs; the publisher's pace; a backend failure
// reaching the interface as an event and a health flag; events posted
// before the interface connects arriving after its Hello.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/conformance.h"
#include "sonora/api/fake_engine.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/session.h"
#include "sonora/state_publisher.h"

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

constexpr int kTracks = 4;

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now().time_since_epoch())
                                     .count());
}

// A session, a fake interface feeding it at real-time pace, and the engine
// on top, owned together so the suite's engine is the whole thing.
class RealEngine final : public api::EngineControl {
public:
    RealEngine(const fs::path& dir, EngineControlServer::Options opts)
        : dir_(dir), session_(config(dir)), audio_(backend_config()) {
        std::string error;
        ok_ = session_.create_dirs() &&
              audio_.open({"sonora", kTracks, 0, 48000, ""}, session_, error) &&
              audio_.activate(error) && audio_.connect_inputs_in_order() == kTracks;
        if (!ok_) std::fprintf(stderr, "  rig: %s\n", error.c_str());
        session_.disk().start();
        engine_ = std::make_unique<InProcessEngine>(session_, &audio_, std::move(opts));
    }
    ~RealEngine() override {
        engine_->shutdown();
        audio_.deactivate();
        session_.stop();
        engine_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    bool connect(std::string& error) override {
        if (!ok_) {
            error = "the rig did not open";
            return false;
        }
        return engine_->connect(error);
    }
    const api::Hello& hello() const override { return engine_->hello(); }
    uint64_t send(const api::CommandRequest& r) override { return engine_->send(r); }
    uint64_t send(const api::ParameterChange& c) override { return engine_->send(c); }
    uint64_t submit(const api::ModelRequest& m) override { return engine_->submit(m); }
    void poll(api::Inbox& in) override { engine_->poll(in); }
    api::StateView& state() override { return engine_->state(); }

    InProcessEngine& engine() { return *engine_; }
    FakeBackend& audio() { return audio_; }
    Session& session() { return session_; }

private:
    static SessionConfig config(const fs::path& dir) {
        SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = kTracks;
        cfg.ring_seconds = 1.0;
        return cfg;
    }
    static FakeBackend::Config backend_config() {
        FakeBackend::Config cfg;
        FakeBackend::Device d = FakeBackend::umc1820();
        d.capture = kTracks;
        cfg.devices = {d};
        cfg.mode = FakeBackend::Mode::Thread;
        return cfg;
    }

    fs::path dir_;
    Session session_;
    FakeBackend audio_;
    std::unique_ptr<InProcessEngine> engine_;
    bool ok_ = false;
};

fs::path fresh_dir(const char* what) {
    static int n = 0;
    fs::path p = fs::temp_directory_path() /
                 ("sonora_test_control_" + std::string(what) + "_" + std::to_string(++n));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

class InProcessRig final : public api::conformance::Rig {
public:
    const char* name() const override { return "InProcessEngine"; }
    std::unique_ptr<api::EngineControl> make(uint32_t protocol, uint32_t layout) override {
        EngineControlServer::Options opts;
        opts.protocol_version = protocol;
        opts.state_layout_version = layout;
        return std::make_unique<RealEngine>(fresh_dir("conformance"), opts);
    }
    uint64_t now_ns() override { return steady_ns(); }
    void wait_ns(uint64_t ns) override {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
    void kill(api::EngineControl& e) override {
        static_cast<RealEngine&>(e).engine().shutdown();
    }
    bool has_signal() const override { return true; }
};

// Polls until `pred` holds or two seconds pass.
template <typename Pred>
bool until(api::EngineControl& e, api::Inbox& in, Pred pred) {
    const auto end = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < end) {
        e.poll(in);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    e.poll(in);
    return pred();
}

void same_commands_as_the_fake() {
    RealEngine real(fresh_dir("commands"), {});
    std::string error;
    CHECK(real.connect(error));
    std::vector<std::string> a = real.hello().implemented_commands;
    std::vector<std::string> b = api::testing::FakeEngine::engine_commands();
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) {
        std::fprintf(stderr,
                     "FAIL FakeEngine::engine_commands() is not what engine/control installs;"
                     " update the fake in the same change as the handlers\n");
        ++failures;
    }
    // app.quit is installed by install_session_commands but belongs to the
    // interface (ADR-0007 §5): never announced, and refused if sent.
    CHECK(std::find(a.begin(), a.end(), "app.quit") == a.end());
    api::CommandRequest r;
    r.command = "app.quit";
    const uint64_t seq = real.send(r);
    api::Inbox in;
    CHECK(until(real, in, [&]() { return !in.replies.empty(); }));
    CHECK(!in.replies.empty() && in.replies[0].seq == seq &&
          in.replies[0].result == DispatchResult::NotImplemented);
}

// Paced, never a loop: at a 1 ms period for 200 ms, about 200 publishes;
// never thousands, which is what starved a reader in the measurement.
void publisher_is_paced() {
    const fs::path dir = fresh_dir("pace");
    SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.ring_seconds = 0.1;
    Session s(cfg);
    api::StateBlockMemory mem(2);
    api::StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), 2, 2, 1'000'000));
    StatePublisher p(s, nullptr, w, 1'000'000);
    const auto t0 = Clock::now();
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    p.stop();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const uint64_t n = p.publishes();
    std::printf("  publisher at 1 ms: %llu publishes in %.0f ms\n",
                static_cast<unsigned long long>(n), ms);
    CHECK(n >= 20);                                  // it runs
    CHECK(n <= static_cast<uint64_t>(ms) + 2);       // and never faster than its period
    // Stopped means stopped: the heartbeat stands still.
    api::StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    const uint64_t hb = v.transport().heartbeat;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(v.transport().heartbeat == hb);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The publisher reads what the audio thread writes and nothing else: an
// armed track and a gain set on the Track show in the block.
void publisher_reads_the_session() {
    const fs::path dir = fresh_dir("reads");
    SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.ring_seconds = 0.1;
    Session s(cfg);
    api::StateBlockMemory mem(2);
    api::StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), 2, 2, 10'000'000));
    StatePublisher p(s, nullptr, w, 10'000'000);
    s.tracks()[1]->set_rec_armed(true);
    s.tracks()[0]->set_gain(0.25f);
    s.tracks()[0]->report_peak(0.5f);
    s.transport().set_playhead(4800);
    p.set_take(7);
    p.set_input_connected(1, true);
    p.publish_once();
    api::StateView v;
    std::string error;
    CHECK(v.attach(mem.data(), mem.bytes(), error));
    p.publish_once();
    CHECK(v.transport().take == 7);
    CHECK(v.transport().playhead == 4800);
    CHECK(!v.track(0).armed() && v.track(1).armed());
    CHECK(v.track(0).gain == 0.25f);
    CHECK(!v.track(0).input_connected() && v.track(1).input_connected());
    // The first publish took the 0.5 from the Track before this reader
    // attached; the publisher is the one caller of take_peak().
    CHECK(v.take_peak(0) == 0.0f);
    s.tracks()[0]->report_peak(0.75f);
    p.publish_once();
    CHECK(v.take_peak(0) == 0.75f);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void fatal_backend_reaches_the_interface() {
    RealEngine real(fresh_dir("fatal"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    real.audio().inject_server_loss();
    const auto fatal = [&]() {
        for (const auto& e : in.events) {
            if (e.kind == api::EventKind::Fatal) return true;
        }
        return false;
    };
    CHECK(until(real, in, fatal));
    for (const auto& e : in.events) {
        if (e.kind == api::EventKind::Fatal) CHECK(!e.text.empty());
    }
    CHECK(until(real, in, [&]() {
        return (real.state().transport().health_flags & api::kHealthServerGone) != 0;
    }));
}

void housekeeping_and_early_events() {
    std::atomic<int> runs{0};
    EngineControlServer::Options opts;
    opts.housekeeping_period_ns = 10'000'000;
    opts.housekeeping = [&runs](EngineControlServer& server) {
        if (runs.fetch_add(1) == 0) {
            api::EngineEvent e;
            e.kind = api::EventKind::InputsMissing;
            e.text = "nothing is connected";
            server.post_event(e);
            server.set_inputs_connected(0, true);
        }
    };
    RealEngine real(fresh_dir("housekeeping"), opts);
    api::EngineEvent early;
    early.kind = api::EventKind::Notice;
    early.text = "recording from Built-in Audio Pro (onboard): no USB interface found";
    real.engine().server().post_event(early);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(until(real, in, [&]() { return in.events.size() >= 2; }));
    CHECK(in.events.size() >= 2 && in.events[0].kind == api::EventKind::Notice &&
          in.events[0].text == early.text);
    CHECK(in.events.size() >= 2 && in.events[1].kind == api::EventKind::InputsMissing);
    CHECK(until(real, in, [&]() {
        return (real.state().transport().health_flags & api::kHealthNoInputs) != 0;
    }));
    CHECK(runs.load() >= 1);
}

}  // namespace

int main() {
    InProcessRig rig;
    const int failed = api::conformance::run(rig);
    if (failed != 0) {
        std::fprintf(stderr, "%d conformance check(s) failed on InProcessEngine\n", failed);
        ++failures;
    }
    same_commands_as_the_fake();
    publisher_is_paced();
    publisher_reads_the_session();
    fatal_backend_reaches_the_interface();
    housekeeping_and_early_events();
    if (failures) return EXIT_FAILURE;
    std::printf("control: ok\n");
    return EXIT_SUCCESS;
}
