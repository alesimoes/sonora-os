// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control API on the real engine (ADR-0007 M4, §7): the conformance
// suite FakeEngine passes, run on InProcessEngine over a real Session fed
// by drivers/fake at real-time pace; FakeEngine's command list held to the
// handlers this build installs; the publisher's pace; a backend failure
// reaching the interface as an event and a health flag; a take that stops
// writing reaching it the same way, from the control thread's check; events
// posted before the interface connects arriving after its Hello.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/conformance.h"
#include "sonora/api/fake_engine.h"
#include "sonora/api/wire.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"
#include "sonora/session.h"
#include "sonora/state_publisher.h"
#include "sonora/wav_writer.h"

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
    // With `watch_inputs`, the engine's housekeeping reports the inputs as
    // engine/app's does: InputsMissing when they go and when they come
    // back, the health flag and the per-track flags. It does not reconnect
    // them; the test decides when they come back.
    RealEngine(const fs::path& dir, EngineControlServer::Options opts, bool watch_inputs = false)
        : dir_(dir), session_(config(dir)), audio_(backend_config()) {
        if (watch_inputs) {
            opts.housekeeping_period_ns = 20'000'000;
            opts.housekeeping = [this](EngineControlServer& server) { watch(server); };
        }
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
    uint64_t send(const api::PluginParameterChange& c) override { return engine_->send(c); }
    uint64_t submit(const api::ModelRequest& m) override { return engine_->submit(m); }
    void poll(api::Inbox& in) override { engine_->poll(in); }
    api::StateView& state() override { return engine_->state(); }

    InProcessEngine& engine() { return *engine_; }
    FakeBackend& audio() { return audio_; }
    Session& session() { return session_; }

    // Track 1's next take stops writing after a few frames: the WAV size
    // limit, lowered, stands in for a full disk. Called while stopped.
    void set_takes_fail(bool fail) {
        session_.tracks()[0]->writer().set_riff_size_limit(fail ? kFailingRiffSize
                                                                : WavWriter::kRiffSizeLimit);
    }
    void set_inputs_present(bool present) {
        if (present) {
            audio_.connect_inputs_in_order();
        } else {
            for (uint32_t i = 0; i < kTracks; ++i) audio_.unpatch(i);
        }
    }

private:
    // The header and a few frames.
    static constexpr uint64_t kFailingRiffSize = 700;

    void watch(EngineControlServer& server) {
        const int n = audio_.connected_input_count();
        const std::string missing = n == 0 ? "no capture input is connected" : "";
        if (missing != inputs_said_) {
            api::EngineEvent e;
            e.kind = api::EventKind::InputsMissing;
            e.text = missing;
            server.post_event(e);
            inputs_said_ = missing;
        }
        server.set_inputs_connected(static_cast<uint32_t>(n), true);
        for (uint32_t i = 0; i < kTracks; ++i) {
            server.set_input_connected(i, audio_.input_connected(i));
        }
    }

    std::string inputs_said_ = "(unset)";   // the control thread's
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
        return std::make_unique<RealEngine>(fresh_dir("conformance"), opts, true);
    }
    uint64_t now_ns() override { return steady_ns(); }
    void wait_ns(uint64_t ns) override {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
    void kill(api::EngineControl& e) override {
        static_cast<RealEngine&>(e).engine().shutdown();
    }
    bool has_signal() const override { return true; }
    bool set_takes_fail(api::EngineControl& e, bool fail) override {
        static_cast<RealEngine&>(e).set_takes_fail(fail);
        return true;
    }
    bool set_inputs_present(api::EngineControl& e, bool present) override {
        static_cast<RealEngine&>(e).set_inputs_present(present);
        return true;
    }
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
    // ADR-0012 §7.3, as the server hands over the playing plan's report.
    p.set_compensation(2048, 1048);
    p.set_track_latency(0, 2048);
    p.set_track_shortfall(1, 1048);
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
    CHECK(v.transport().compensation_frames == 2048);
    CHECK(v.transport().compensation_shortfall_frames == 1048);
    CHECK(v.track(0).latency_frames == 2048 && v.track(1).latency_frames == 0);
    CHECK(!v.track(0).compensation_short() && v.track(1).compensation_short());
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

// The take-failure check moved from the terminal loop into the server's
// control thread (EngineControlServer::tick). A writer that fails mid-take
// reaches the interface with the file and the writer's reason, once, and
// the health flag holds until a take records cleanly.
void take_failure_reaches_the_interface() {
    RealEngine real(fresh_dir("take_failed"), {});
    std::string error;
    CHECK(real.connect(error));
    real.set_takes_fail(true);
    api::Inbox in;
    const auto replied = [&](uint64_t seq, DispatchResult want) {
        return until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) return r.result == want;
            }
            return false;
        });
    };
    const auto failures_said = [&]() {
        std::vector<api::EngineEvent> out;
        for (const auto& e : in.events) {
            if (e.kind == api::EventKind::TakeFailed) out.push_back(e);
        }
        return out;
    };
    api::CommandRequest arm;
    arm.command = "track.toggle_arm";
    arm.target = 0;
    CHECK(replied(real.send(arm), DispatchResult::Ok));
    api::CommandRequest rec;
    rec.command = "transport.record";
    CHECK(replied(real.send(rec), DispatchResult::Ok));
    CHECK(until(real, in, [&]() { return !failures_said().empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // four checks more
    real.poll(in);
    const auto said = failures_said();
    CHECK(said.size() == 1);
    if (!said.empty()) {
        CHECK(said[0].text.find("Audio 1_01.wav") != std::string::npos);
        CHECK(said[0].text.find("4 GB WAV limit") != std::string::npos);
        CHECK(said[0].frame == 1);
    }
    CHECK(until(real, in, [&]() {
        return (real.state().transport().health_flags & api::kHealthTakeFailed) != 0;
    }));
    api::CommandRequest stop;
    stop.command = "transport.stop";
    CHECK(replied(real.send(stop), DispatchResult::Failed));

    real.set_takes_fail(false);
    CHECK(replied(real.send(rec), DispatchResult::Ok));
    CHECK(until(real, in, [&]() {
        return failures_said().size() == 2 &&
               (real.state().transport().health_flags & api::kHealthTakeFailed) == 0;
    }));
    const auto all = failures_said();
    CHECK(all.size() == 2 && all[1].text.empty() && all[1].frame == 2);
    CHECK(replied(real.send(stop), DispatchResult::Ok));
}

model::ReplicationUpdate from_wire(const api::ModelUpdate& m);

// ADR-0016 end to end: with the document's loop range set, transport.record
// starts a loop take; while it records, a SetSettings that moves the loop
// range or clears it is refused by name, as is opening a session, and any
// other setting still applies; stopped after a wrap, its passes reach the
// interface's replica on playlists of their own.
void loop_record_on_the_real_engine() {
    RealEngine real(fresh_dir("loop_record"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    model::Replica replica;
    std::size_t fed = 0;
    const auto feed = [&]() {
        for (; fed < in.model.size(); ++fed) {
            std::string why;
            CHECK(replica.receive(from_wire(in.model[fed]), why));
        }
    };
    const auto reply_to = [&](uint64_t seq, api::CommandReply& out) {
        const bool got = until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) {
                    out = r;
                    return true;
                }
            }
            return false;
        });
        feed();
        return got;
    };
    CHECK(until(real, in, [&]() { return !in.model.empty(); }));
    feed();
    CHECK(replica.synced());
    if (!replica.synced()) return;
    const auto settings = [&](std::array<model::Frames, 2> loop, model::Frames post_roll) {
        model::SessionSettings st = replica.current().settings;
        st.transport.loop = loop;
        st.transport.pre_roll = 0;
        st.transport.post_roll = post_roll;
        api::ModelRequest m;
        m.action = api::ModelAction::Apply;
        std::string e;
        model::encode_operation(model::SetSettings{st}, m.op, e);
        api::CommandReply r;
        reply_to(real.submit(m), r);
        return r;
    };
    // One second of loop from the first second; no pre-roll.
    CHECK(settings({48000, 96000}, 48000).result == DispatchResult::Ok);
    api::CommandReply r;
    api::CommandRequest arm;
    arm.command = "track.toggle_arm";
    arm.target = 0;
    CHECK(reply_to(real.send(arm), r) && r.result == DispatchResult::Ok);
    api::CommandRequest rec;
    rec.command = "transport.record";
    CHECK(reply_to(real.send(rec), r) && r.result == DispatchResult::Ok);
    CHECK(until(real, in, [&]() {
        return real.state().transport().state == api::TransportState::Recording;
    }));
    CHECK(real.session().loop_take_running());

    r = settings({48000, 100000}, 48000);
    CHECK(r.result == DispatchResult::Failed);
    CHECK(r.message.find("the loop range cannot change while loop record runs (from frame "
                         "48000 to frame 96000)") != std::string::npos);
    CHECK(settings({0, 0}, 48000).result == DispatchResult::Failed);
    CHECK(settings({48000, 96000}, 24000).result == DispatchResult::Ok);
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    CHECK(model::serialize(replica.current(), model::WriteInfo{"test", 1}, load.op, error));
    CHECK(reply_to(real.submit(load), r) && r.result == DispatchResult::Failed);
    CHECK(r.message.find("while loop record runs") != std::string::npos);

    // Past B, so the take has a whole pass.
    CHECK(until(real, in, [&]() {
        const uint64_t p = real.state().transport().playhead;
        return p >= 48000 && p < 60000 && real.session().take_origin() == 48000;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    api::CommandRequest stop;
    stop.command = "transport.stop";
    CHECK(reply_to(real.send(stop), r) && r.result == DispatchResult::Ok);
    CHECK(until(real, in, [&]() {
        feed();
        return replica.synced() && replica.current().edit->tracks[0]->playlists.size() >= 3;
    }));
    const model::TrackEdit& t = *replica.current().edit->tracks[0];
    if (t.playlists.size() < 3) return;
    CHECK(t.playlists[1]->name == t.name + ".T1.P1");
    CHECK(t.playlists[1]->regions.size() == 1);
    CHECK(t.playlists[1]->regions[0].position == 48000);
    CHECK(t.playlists[1]->regions[0].length == 48000);
    // The pass is on the active playlist too.
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) CHECK(p->regions.size() == 1);
    }
}

model::ReplicationUpdate from_wire(const api::ModelUpdate& m) {
    model::ReplicationUpdate u;
    u.kind = m.kind == api::ModelUpdateKind::Op     ? model::ReplicationUpdate::Kind::Op
             : m.kind == api::ModelUpdateKind::Undo ? model::ReplicationUpdate::Kind::Undo
             : m.kind == api::ModelUpdateKind::Redo ? model::ReplicationUpdate::Kind::Redo
                                                    : model::ReplicationUpdate::Kind::Snapshot;
    u.revision = m.revision;
    u.undo_floor = m.undo_floor;
    u.next_id = m.next_id;
    u.bytes = m.bytes;
    u.label = m.label;
    return u;
}

// A Snapshot far over one datagram (an AF_UNIX send buffer is about
// 208 KiB by default) crosses whole, in frames, and a replica that resyncs
// from it undoes and redoes with the engine.
void large_snapshot_crosses_in_frames() {
    RealEngine real(fresh_dir("large_snapshot"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    model::Replica replica;
    std::size_t fed = 0;
    std::string replica_error;
    const auto feed = [&]() {
        for (; fed < in.model.size(); ++fed) {
            std::string why;
            if (!replica.receive(from_wire(in.model[fed]), why)) replica_error = why;
        }
    };
    const auto settle = [&](std::size_t updates) {
        const bool ok = until(real, in, [&]() { return in.model.size() >= updates; });
        feed();
        return ok;
    };
    CHECK(settle(1));
    CHECK(replica.synced());
    uint64_t next_id = replica.current().next_id;
    // Each step's marker list holds every marker so far, so the window
    // grows with the square of the count.
    constexpr int kMarkers = 80;
    for (int i = 0; i < kMarkers; ++i) {
        model::AddMarker add;
        add.marker.id = next_id++;
        add.marker.position = 48000 * i;
        add.marker.name = "marker " + std::to_string(i) + " " + std::string(400, 'x');
        api::ModelRequest m;
        CHECK(model::encode_operation(add, m.op, error));
        real.submit(m);
    }
    CHECK(settle(1 + kMarkers));
    api::ModelRequest resync;
    resync.action = api::ModelAction::Resync;
    real.submit(resync);
    CHECK(settle(2 + kMarkers));
    CHECK(in.model.size() == 2 + kMarkers);
    const api::ModelUpdate& snap = in.model.back();
    CHECK(snap.kind == api::ModelUpdateKind::Snapshot);
    constexpr std::size_t kRoom = api::wire::kMaxFrameBytes - api::wire::kFrameHeaderBytes;
    std::printf("  a Snapshot of %zu bytes crossed in %zu frames\n", snap.bytes.size(),
                (snap.bytes.size() + kRoom - 1) / kRoom);
    CHECK(snap.bytes.size() > 4 * api::wire::kMaxFrameBytes);
    CHECK(replica.synced() && replica_error.empty());
    CHECK(replica.can_undo());
    api::ModelRequest undo;
    undo.action = api::ModelAction::Undo;
    for (int i = 0; i < 5; ++i) real.submit(undo);
    CHECK(settle(7 + kMarkers));
    CHECK(replica.synced() && replica_error.empty());
    CHECK(replica.current().edit->markers->markers.size() == kMarkers - 5);
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

// The events a client attaching now is greeted with: every one that stands.
std::vector<api::EngineEvent> late_greeting(RealEngine& real, api::Inbox& in) {
    struct Got {
        std::mutex mutex;
        std::vector<std::string> frames;
    };
    auto got = std::make_shared<Got>();
    EngineControlServer& server = real.engine().server();
    const auto id = server.attach(
        [got](EngineControlServer::Frame frame) {
            std::lock_guard<std::mutex> lock(got->mutex);
            got->frames.push_back(*frame);
        },
        {});
    // The control thread takes the attach before a request delivered after
    // it, so once that request is answered the greeting is all there.
    api::CommandRequest r;
    r.command = "no.such.command";
    const uint64_t seq = real.send(r);
    CHECK(until(real, in, [&]() {
        for (const auto& reply : in.replies) {
            if (reply.seq == seq) return true;
        }
        return false;
    }));
    server.detach(id);
    std::vector<std::string> frames;
    {
        std::lock_guard<std::mutex> lock(got->mutex);
        frames = got->frames;
    }
    api::wire::Reassembler reassembler;
    api::Inbox late;
    std::string message, error;
    for (const std::string& f : frames) {
        if (reassembler.add(f, message, error) == api::wire::Reassembler::Result::Complete) {
            api::wire::decode_into(message, late, error);
        }
    }
    return late.events;
}

std::size_t plugin_faults_in(const std::vector<api::EngineEvent>& events) {
    std::size_t n = 0;
    for (const auto& e : events) n += e.kind == api::EventKind::PluginFault ? 1 : 0;
    return n;
}

// Until engine/plugin_client lands this engine hosts nothing, and says so
// at every door: an empty catalog, NotImplemented for whatever would start
// a host, the health flag, and every insert of a session it opens shown
// faulted with a PluginFault for each active one (ADR-0012 §5.5: "never
// silently dry").
void no_plugin_hosting_yet() {
    RealEngine real(fresh_dir("plugins"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    model::Replica replica;
    std::size_t fed = 0;
    const auto feed = [&]() {
        for (; fed < in.model.size(); ++fed) {
            const api::ModelUpdate& m = in.model[fed];
            model::ReplicationUpdate u;
            u.kind = m.kind == api::ModelUpdateKind::Snapshot
                         ? model::ReplicationUpdate::Kind::Snapshot
                         : model::ReplicationUpdate::Kind::Op;
            u.revision = m.revision;
            u.undo_floor = m.undo_floor;
            u.next_id = m.next_id;
            u.bytes = m.bytes;
            u.label = m.label;
            std::string why;
            CHECK(replica.receive(u, why));
        }
        return replica.synced();
    };
    const auto reply_to = [&](uint64_t seq, api::CommandReply& out) {
        const bool got = until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) {
                    out = r;
                    return true;
                }
            }
            return false;
        });
        feed();
        return got;
    };
    CHECK(until(real, in, [&]() { return feed() && !in.catalogs.empty(); }));
    CHECK(!in.catalogs.empty() && in.catalogs[0].plugins.empty());
    CHECK(until(real, in, [&]() {
        return (real.state().transport().health_flags & api::kHealthPluginsDisabled) != 0;
    }));
    if (!replica.synced()) return;

    const model::PluginRef ref{model::PluginFormat::Lv2, "http://gareus.org/oss/lv2/dpl#mono",
                               "x42-dpl", "0.9.2"};
    model::AddInsert add;
    add.track = replica.current().edit->tracks[0]->id;
    add.insert = replica.current().next_id;
    add.plugin = ref;
    api::ModelRequest m;
    m.action = api::ModelAction::Apply;
    CHECK(model::encode_operation(add, m.op, error));
    api::CommandReply r;
    CHECK(reply_to(real.submit(m), r));
    CHECK(r.result == DispatchResult::NotImplemented);
    CHECK(r.message.find("engine/plugin_client") != std::string::npos);
    api::PluginParameterChange c;
    c.insert = add.insert;
    c.key = "threshold";
    CHECK(reply_to(real.send(c), r));
    CHECK(r.result == DispatchResult::NotImplemented);

    // A session that already holds inserts opens: recording must not depend
    // on a plugin. Both are kept and shown faulted; the active one is named.
    model::Snapshot doc = replica.current();
    auto edit = std::make_shared<model::EditState>(*doc.edit);
    auto track = std::make_shared<model::TrackEdit>(*edit->tracks[0]);
    const model::Id active = doc.next_id;
    const model::Id inactive = doc.next_id + 1;
    track->inserts = {model::Insert{active, 0, ref, true}, model::Insert{inactive, 4, ref, false}};
    edit->tracks[0] = track;
    doc.edit = edit;
    doc.mix.inserts = {model::InsertMix{active, true, {}, {}},
                       model::InsertMix{inactive, false, {}, {}}};
    doc.next_id += 2;
    std::string bytes;
    CHECK(model::serialize(doc, model::WriteInfo{"test", 1}, bytes, error));
    m.action = api::ModelAction::Load;
    m.op = bytes;
    const std::size_t events = in.events.size();
    CHECK(reply_to(real.submit(m), r) && r.result == DispatchResult::Ok);
    CHECK(until(real, in, [&]() {
        const api::TrackSnapshot t = real.state().track(0);
        return t.inserts[0].present() && t.inserts[4].present();
    }));
    const api::TrackSnapshot t = real.state().track(0);
    CHECK(t.inserts[0].faulted() && (t.inserts[0].flags & api::kInsertActive) != 0);
    CHECK((t.inserts[0].flags & api::kInsertBypassed) != 0);
    CHECK((t.inserts[0].flags & api::kInsertRunning) == 0);
    CHECK(t.inserts[4].faulted() && (t.inserts[4].flags & api::kInsertActive) == 0);
    std::size_t faults = 0;
    for (std::size_t i = events; i < in.events.size(); ++i) {
        if (in.events[i].kind != api::EventKind::PluginFault) continue;
        ++faults;
        CHECK(in.events[i].insert == active && in.events[i].track == -1);
        CHECK(in.events[i].text.find("x42-dpl (") != std::string::npos &&
              in.events[i].text.find("slot A) is not running") != std::string::npos);
    }
    CHECK(faults == 1);

    // Opened again and again, the session's fault stands once: neither the
    // last session's nor a copy per open is replayed to a later client, and
    // none takes a device notice's place (rt-engineer review of #46).
    for (int i = 0; i < 40; ++i) {
        CHECK(reply_to(real.submit(m), r) && r.result == DispatchResult::Ok);
    }
    std::vector<api::EngineEvent> late = late_greeting(real, in);
    CHECK(plugin_faults_in(late) == 1);
    for (const auto& e : late) {
        if (e.kind == api::EventKind::PluginFault) CHECK(e.insert == active && !e.text.empty());
    }

    // Edits that start no host apply: the insert goes out of the chain, and
    // its fault is cleared, for this client and for a later one.
    m.action = api::ModelAction::Apply;
    CHECK(model::encode_operation(model::SetInsertActive{active, false}, m.op, error));
    CHECK(reply_to(real.submit(m), r) && r.result == DispatchResult::Ok);
    CHECK(until(real, in, [&]() {
        return (real.state().track(0).inserts[0].flags & api::kInsertActive) == 0;
    }));
    CHECK(until(real, in, [&]() {
        if (in.events.empty()) return false;
        const api::EngineEvent& e = in.events.back();
        return e.kind == api::EventKind::PluginFault && e.insert == active && e.text.empty();
    }));
    late = late_greeting(real, in);
    CHECK(plugin_faults_in(late) == 0);
    // FlushPluginState: nothing runs, so nothing to record, and Ok.
    api::ModelRequest flush;
    flush.action = api::ModelAction::FlushPluginState;
    flush.op = std::to_string(active);
    CHECK(reply_to(real.submit(flush), r) && r.result == DispatchResult::Ok);

    // A fader ride survives a change to an insert's values: only the
    // faders' part of the mix goes to the Track atomics.
    api::ParameterChange ride;
    ride.track = 0;
    ride.value = 0.5f;
    CHECK(reply_to(real.send(ride), r) && r.result == DispatchResult::Ok);
    CHECK(model::encode_operation(model::SetInsertBypass{inactive, true}, m.op, error));
    CHECK(reply_to(real.submit(m), r) && r.result == DispatchResult::Ok);
    CHECK(until(real, in, [&]() {
        return (real.state().track(0).inserts[4].flags & api::kInsertBypassed) != 0;
    }));
    CHECK(real.state().track(0).gain == 0.5f);
}

// A ride the link lost mid-gesture stays in the Track atomic, a value the
// document does not hold. A Resync applies the document's mix (ADR-0009 §4
// item 3), and so does the rider leaving, for the tracks it rode; another
// client attaching mid-ride leaves the ride alone.
void resync_applies_the_mix() {
    RealEngine real(fresh_dir("resync"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto answered = [&](uint64_t seq) {
        return until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) return r.result == DispatchResult::Ok;
            }
            return false;
        });
    };
    const auto gain_is = [&](float g) {
        return until(real, in, [&]() { return real.state().track(0).gain == g; });
    };
    api::ParameterChange ride;
    ride.track = 0;
    ride.value = 0.5f;
    CHECK(answered(real.send(ride)));
    CHECK(gain_is(0.5f));
    api::ModelRequest resync;
    resync.action = api::ModelAction::Resync;
    CHECK(answered(real.submit(resync)));
    CHECK(gain_is(1.0f));

    // Everything delivered so far handled, and a publish begun and ended
    // since: what the block says is the server's state after it.
    const auto settled = [&]() {
        api::CommandRequest probe;
        probe.command = "no.such.command";
        const uint64_t seq = real.send(probe);
        const bool got = until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) return true;
            }
            return false;
        });
        const uint64_t hb = real.state().transport().heartbeat;
        return got && until(real, in, [&]() {
                   return real.state().transport().heartbeat >= hb + 2;
               });
    };

    // Another interface attaching mid-ride does not snap the ride back,
    // nor does its leaving: it never rode (review of PR #71).
    ride.value = 0.25f;
    CHECK(answered(real.send(ride)));
    CHECK(gain_is(0.25f));
    late_greeting(real, in);
    CHECK(settled());
    CHECK(real.state().track(0).gain == 0.25f);

    // The rider leaving mid-ride ends its ride: the document's gain is put
    // back. Here a second client rides track 1 and goes with no release.
    EngineControlServer& server = real.engine().server();
    const auto rider = server.attach([](EngineControlServer::Frame) {}, {});
    api::ParameterChange stranded;
    stranded.seq = 1;
    stranded.track = 1;
    stranded.value = 0.75f;
    std::string bytes;
    api::wire::encode(stranded, bytes);
    std::vector<std::string> frames;
    CHECK(api::wire::split_frames(bytes, frames, error));
    for (std::string& f : frames) server.deliver(rider, std::move(f));
    CHECK(until(real, in, [&]() { return real.state().track(1).gain == 0.75f; }));
    server.detach(rider);
    CHECK(until(real, in, [&]() { return real.state().track(1).gain == 1.0f; }));
    // Only the tracks it rode: track 0's ride, the first client's, stays.
    CHECK(settled());
    CHECK(real.state().track(0).gain == 0.25f);
}

}  // namespace

// ADR-0015 §5 on the real engine: Options::midi_max_entries is the budget,
// a session over it is refused at open by name, whole, and one at 90 % of
// it opens with a Notice, so the operator hears of it before a take.
void midi_budget_at_open_and_its_notice() {
    EngineControlServer::Options opts;
    opts.midi_max_entries = 10;
    RealEngine real(fresh_dir("midi_budget"), opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    // One MIDI track whose one region holds `n` controller events, as the
    // interface sends a session: a bundle with its sequence file.
    const auto bundle = [&](int n) {
        model::SessionSettings settings;
        settings.name = "Budget";
        model::History h(model::new_session(settings));
        model::AddTrack t;
        t.track = 1;
        t.playlist = 2;
        t.name = "Bass";
        t.playlist_name = "Bass.01";
        t.kind = model::TrackKind::Midi;
        t.channels = 0;
        auto seq = std::make_shared<model::MidiSequence>();
        for (int i = 0; i < n; ++i) seq->events.push_back(model::MidiEvent{i, 0xB0, 1, 2});
        model::MidiRegion r;
        r.id = 3;
        r.name = "Bass_01";
        r.length = model::kTicksPerQuarter;
        r.data = seq;
        std::string out, why;
        CHECK(h.apply(t, why) && h.apply(model::PlaceMidiRegion{2, r, 0, {}}, why));
        CHECK(model::encode_session_bundle(h.current(), model::WriteInfo{"test", 1}, out, why));
        return out;
    };
    const auto load = [&](int n, api::CommandReply& out) {
        api::ModelRequest m;
        m.action = api::ModelAction::Load;
        m.op = bundle(n);
        const uint64_t seq = real.submit(m);
        return until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq != seq) continue;
                out = r;
                return true;
            }
            return false;
        });
    };
    api::CommandReply r;
    CHECK(load(11, r) && r.result == DispatchResult::Failed);
    CHECK(r.message.find("holds 11 MIDI notes and events") != std::string::npos);
    CHECK(r.message.find("midi_max_entries of 10; raise the setting") != std::string::npos);
    CHECK(load(9, r) && r.result == DispatchResult::Ok);
    CHECK(until(real, in, [&]() {
        for (const auto& e : in.events) {
            if (e.kind == api::EventKind::Notice &&
                e.text.find("MIDI holds 9 of the 10 notes and events") != std::string::npos) {
                return true;
            }
        }
        return false;
    }));
}

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
    take_failure_reaches_the_interface();
    loop_record_on_the_real_engine();
    large_snapshot_crosses_in_frames();
    housekeeping_and_early_events();
    no_plugin_hosting_yet();
    resync_applies_the_mix();
    midi_budget_at_open_and_its_notice();
    if (failures) return EXIT_FAILURE;
    std::printf("control: ok\n");
    return EXIT_SUCCESS;
}
