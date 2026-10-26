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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/active_folder.h"
#include "sonora/api/conformance.h"
#include "sonora/api/fake_engine.h"
#include "sonora/api/wire.h"
#include "sonora/closed_take.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/machine_latency.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"
#include "sonora/platform/file.h"
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
    // With `untitled`, `dir` is entered as engine/app enters the folder it
    // made for a new session (ADR-0018 §3), through an ActiveFolder of the
    // rig's own.
    RealEngine(const fs::path& dir, EngineControlServer::Options opts, bool watch_inputs = false,
               bool untitled = false)
        : dir_(dir), session_(config(dir)), audio_(backend_config()) {
        if (watch_inputs) {
            opts.housekeeping_period_ns = 20'000'000;
            opts.housekeeping = [this](EngineControlServer& server) { watch(server); };
        }
        std::string error;
        if (untitled) {
            std::vector<std::string> notices;
            if (!folder_.enter(session_, dir.string(), ActiveFolder::Kind::Untitled, error,
                               notices)) {
                std::fprintf(stderr, "  rig: %s\n", error.c_str());
            }
            opts.active_folder = &folder_;
        }
        ok_ = session_.create_dirs() &&
              audio_.open({"sonora", kTracks, 2, 48000, ""}, session_, error) &&
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
    // The document the engine would start with in `dir`: the rig's
    // tracks, and a session id a test can write into a take beforehand.
    static model::Snapshot start_document(const fs::path& dir) {
        Session probe(config(dir));
        return engine_document(probe);
    }
    // A take as a crash leaves it: written and closed by this engine's
    // writer, with its header, and in no source.
    bool leave_orphan_take(const std::string& name, const std::string& session,
                           const std::string& session_id, int take, uint64_t track_id,
                           uint64_t position, uint64_t frames) {
        WavWriter w;
        w.set_description(take_description(take, track_id, session, nullptr, nullptr, session_id));
        if (!w.open((dir_ / "Audio Files" / name).string(), 48000, 1, WavWriter::Format::Int24,
                    position)) {
            return false;
        }
        const std::vector<float> audio(frames, 0.25f);
        return w.write(audio.data(), audio.size()) && w.close();
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

public:
    static constexpr uint32_t kCableFrames = 300;

private:
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
        // The latency probe's cable (ADR-0019, update of 2026-10-26): out_2
        // back into the last input.
        cfg.loopbacks = {{1, 0, kTracks - 1, kCableFrames}};
        return cfg;
    }

    fs::path dir_;
    Session session_;
    FakeBackend audio_;
    ActiveFolder folder_;
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
    bool leave_orphan_take(api::EngineControl& e, const std::string& name,
                           const std::string& session, const std::string& session_id, int take,
                           uint64_t track_id, uint64_t position, uint64_t frames) override {
        return static_cast<RealEngine&>(e).leave_orphan_take(name, session, session_id, take,
                                                             track_id, position, frames);
    }
    bool latency_cable(uint32_t& output, uint32_t& input, uint32_t& frames) const override {
        output = 2;
        input = kTracks;
        frames = RealEngine::kCableFrames;
        return true;
    }
    int take_files_in(api::EngineControl&, const std::string& folder, int take) override {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "_%02d.wav", take);
        const std::string tail = suffix;
        int files = 0;
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(folder) / "Audio Files", ec), end;
             !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() > tail.size() &&
                name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
                ++files;
            }
        }
        return files;
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
    // An interface sends its selection as play's range only to an engine
    // announcing it (ADR-0007 §6); the fake announces what the engine does.
    CHECK(real.hello().capabilities ==
          std::vector<std::string>{api::kCapabilityPlayRange});
    CHECK(real.hello().capabilities == api::testing::FakeEngine::Config{}.capabilities);
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

// A take left on disk is offered to the first interface to attach, from
// the start (session-format.md, "Crash recovery"). The take being written,
// its header already checkpointed with audio, is in no source yet and must
// never be offered: a session open during it is refused, and once it
// closes it goes in.
void recovery_offer_at_start_and_after_a_take() {
    const fs::path dir = fresh_dir("orphans");
    fs::create_directories(dir / "Audio Files");
    EngineControlServer::Options opts;
    const model::Snapshot start = RealEngine::start_document(dir);
    opts.initial_document = std::make_shared<model::Snapshot>(start);
    {
        WavWriter w;
        w.set_description(
            take_description(5, 0, "Untitled", nullptr, nullptr, start.settings.session_id));
        CHECK(w.open((dir / "Audio Files" / "Audio 3_05.wav").string(), 48000, 1,
                     WavWriter::Format::Int24, 0));
        const std::vector<float> audio(4800, 0.25f);
        CHECK(w.write(audio.data(), audio.size()) && w.close());
    }
    RealEngine real(dir, opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto replied = [&](uint64_t seq, DispatchResult want) {
        return until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) return r.result == want;
            }
            return false;
        });
    };
    const auto offers = [&]() {
        std::vector<std::string> out;
        for (const auto& e : in.events) {
            if (e.kind == api::EventKind::RecoveryOffer) out.push_back(e.text);
        }
        return out;
    };
    CHECK(until(real, in, [&]() { return !offers().empty(); }));
    CHECK(offers().size() == 1 &&
          offers()[0].find("Audio 3_05.wav: take 5, 0.100 s, into the audio pool only") !=
              std::string::npos);

    api::CommandRequest arm;
    arm.command = "track.toggle_arm";
    arm.target = 0;
    CHECK(replied(real.send(arm), DispatchResult::Ok));
    api::CommandRequest rec;
    rec.command = "transport.record";
    CHECK(replied(real.send(rec), DispatchResult::Ok));
    // Past the first checkpoint (SessionConfig::sync_seconds), so the open
    // take's header counts audio.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    CHECK(model::serialize(start, model::WriteInfo{"test", 1}, load.op, error));
    load.op = api::wire::encode_load("", load.op);
    // No session opens during a take (ADR-0018 §2), so the take being
    // written is never another session's orphan; the offer stands as it was.
    CHECK(replied(real.submit(load), DispatchResult::Failed));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    real.poll(in);
    CHECK(offers().size() == 1);

    const std::size_t updates = in.model.size();
    api::CommandRequest stop;
    stop.command = "transport.stop";
    CHECK(replied(real.send(stop), DispatchResult::Ok));
    // Placed, its RecordTake sent, and not offered.
    CHECK(until(real, in, [&]() { return in.model.size() > updates; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    real.poll(in);
    CHECK(offers().size() == 1);
}

// PR #132 review 2: a take punched out on the fly is not placed at the
// punch-out. Its files stay open until the disk thread hears of it, while
// recording_take() is already 0, and its header, checkpointed with audio,
// names no source yet. No session opens during the take (ADR-0018 §2), nor
// while it closes (Session::folder_change_refusal()), so it is never
// offered.
void a_take_just_punched_out_is_never_offered() {
    const fs::path dir = fresh_dir("orphans_punch");
    EngineControlServer::Options opts;
    const model::Snapshot start = RealEngine::start_document(dir);
    opts.initial_document = std::make_shared<model::Snapshot>(start);
    RealEngine real(dir, opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto replied = [&](uint64_t seq, DispatchResult want) {
        return until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq == seq) return r.result == want;
            }
            return false;
        });
    };
    const auto offered = [&]() {
        for (const auto& e : in.events) {
            if (e.kind == api::EventKind::RecoveryOffer && !e.text.empty()) return true;
        }
        return false;
    };
    api::CommandRequest arm;
    arm.command = "track.toggle_arm";
    arm.target = 0;
    CHECK(replied(real.send(arm), DispatchResult::Ok));
    api::CommandRequest rec;
    rec.command = "transport.record";
    CHECK(replied(real.send(rec), DispatchResult::Ok));
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    CHECK(model::serialize(start, model::WriteInfo{"test", 1}, load.op, error));
    load.op = api::wire::encode_load("", load.op);
    CHECK(replied(real.submit(load), DispatchResult::Failed));
    // Past the first checkpoint (SessionConfig::sync_seconds), so the
    // take's header counts audio.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    real.poll(in);
    const std::size_t updates = in.model.size();
    api::CommandRequest punch;
    punch.command = "transport.punch";
    CHECK(replied(real.send(punch), DispatchResult::Ok));
    // Placed: its RecordTake arrives.
    CHECK(until(real, in, [&]() { return in.model.size() > updates; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    real.poll(in);
    CHECK(!offered());
    api::CommandRequest stop;
    stop.command = "transport.stop";
    CHECK(replied(real.send(stop), DispatchResult::Ok));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    real.poll(in);
    CHECK(!offered());
}

// ADR-0015 §6 on the real engine: a Save while a take is open is said to
// wait, and is answered only after the disk thread's next checkpoint; with
// the transport stopped it is answered at once.
void a_save_during_a_take_follows_a_checkpoint() {
    const fs::path dir = fresh_dir("save_take");
    RealEngine real(dir, {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto answer = [&](uint64_t seq, std::chrono::seconds within) -> const api::CommandReply* {
        const auto end = Clock::now() + within;
        while (Clock::now() < end) {
            real.poll(in);
            for (const auto& r : in.replies) {
                if (r.seq == seq) return &r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return nullptr;
    };
    model::SessionSettings settings;
    settings.name = "Saved";
    std::string bytes;
    CHECK(model::serialize(model::new_session(settings), model::WriteInfo{"test", 1}, bytes, error));
    const fs::path file = dir / "Saved" / "Saved.sonora";
    api::ModelRequest save;
    save.action = api::ModelAction::Save;
    save.op = api::wire::encode_save(file.string(), bytes);

    api::CommandRequest arm;
    arm.command = "track.toggle_arm";
    arm.target = 0;
    const api::CommandReply* r = answer(real.send(arm), std::chrono::seconds(2));
    CHECK(r != nullptr && r->result == DispatchResult::Ok);
    api::CommandRequest rec;
    rec.command = "transport.record";
    r = answer(real.send(rec), std::chrono::seconds(2));
    CHECK(r != nullptr && r->result == DispatchResult::Ok);

    const uint32_t before = real.session().disk().checkpoints();
    r = answer(real.submit(save), std::chrono::seconds(6));   // sync_seconds is 2
    CHECK(r != nullptr && r->result == DispatchResult::Ok);
    CHECK(real.session().disk().checkpoints() > before);
    CHECK(fs::exists(file));
    const bool said = std::any_of(in.events.begin(), in.events.end(), [](const api::EngineEvent& e) {
        return e.kind == api::EventKind::Notice && e.text == "Saving after the next checkpoint";
    });
    CHECK(said);

    api::CommandRequest stop;
    stop.command = "transport.stop";
    r = answer(real.send(stop), std::chrono::seconds(2));
    CHECK(r != nullptr && r->result == DispatchResult::Ok);
    in.events.clear();
    const auto asked = Clock::now();
    r = answer(real.submit(save), std::chrono::seconds(2));
    CHECK(r != nullptr && r->result == DispatchResult::Ok);
    CHECK(Clock::now() - asked < std::chrono::seconds(1));
    CHECK(std::none_of(in.events.begin(), in.events.end(), [](const api::EngineEvent& e) {
        return e.text == "Saving after the next checkpoint";
    }));
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
        st.transport.loop_playback = true;
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
    // The session's round trip reaches the loop take (ADR-0019 §4).
    constexpr uint32_t kLatency = 240;
    CHECK(real.session().set_input_latency(kLatency, &error));
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
    load.op = api::wire::encode_load("", load.op);
    CHECK(reply_to(real.submit(load), r) && r.result == DispatchResult::Failed);
    CHECK(r.message.find("while loop record runs") != std::string::npos);
    // Nor can loop playback turn off under the take's wraps.
    api::CommandRequest toggle_loop;
    toggle_loop.command = "transport.toggle_loop";
    CHECK(reply_to(real.send(toggle_loop), r) && r.result == DispatchResult::Failed);
    CHECK(r.message.find("loop playback cannot change while loop record runs") !=
          std::string::npos);
    feed();
    CHECK(replica.current().settings.transport.loop_playback);

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
    std::size_t recorded = 0;
    for (const model::Source& src : replica.current().sources->sources) {
        if (src.kind != model::SourceKind::Recorded) continue;
        ++recorded;
        CHECK(src.latency_compensation == kLatency);
    }
    CHECK(recorded >= 1);
}

model::ReplicationUpdate from_wire(const api::ModelUpdate& m) {
    model::ReplicationUpdate u;
    // The two enums list the same kinds in the same order.
    u.kind = static_cast<model::ReplicationUpdate::Kind>(m.kind);
    u.part_count = m.part_count;
    u.part_bytes = m.part_bytes;
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
    // The standing SessionFolder (ADR-0018 §4) and Latency (ADR-0019) are
    // not what this is about.
    const auto said = [&]() {
        std::vector<api::EngineEvent> out;
        for (const auto& e : in.events) {
            if (e.kind != api::EventKind::SessionFolder && e.kind != api::EventKind::Latency) {
                out.push_back(e);
            }
        }
        return out;
    };
    CHECK(until(real, in, [&]() { return said().size() >= 2; }));
    CHECK(said().size() >= 2 && said()[0].kind == api::EventKind::Notice &&
          said()[0].text == early.text);
    CHECK(said().size() >= 2 && said()[1].kind == api::EventKind::InputsMissing);
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
            // The two enums list the same kinds in the same order.
            u.kind = static_cast<model::ReplicationUpdate::Kind>(m.kind);
            u.part_count = m.part_count;
            u.part_bytes = m.part_bytes;
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
    m.op = api::wire::encode_load("", bytes);
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
        model::MidiLists seq;
        for (int i = 0; i < n; ++i) seq.events.push_back(model::MidiEvent{i, 0xB0, 1, 2});
        model::MidiRegion r;
        r.id = 3;
        r.name = "Bass_01";
        r.length = model::kTicksPerQuarter;
        r.data = std::make_shared<const model::MidiSequence>(seq);
        std::string out, why;
        CHECK(h.apply(t, why) && h.apply(model::PlaceMidiRegion{2, r, 0, {}}, why));
        CHECK(model::encode_session_bundle(h.current(), model::WriteInfo{"test", 1}, out, why));
        return out;
    };
    const auto load = [&](int n, api::CommandReply& out) {
        api::ModelRequest m;
        m.action = api::ModelAction::Load;
        m.op = api::wire::encode_load("", bundle(n));
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

bool wait_for(const std::function<bool()>& pred) {
    for (int i = 0; i < 2000 && !pred(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// Opens a session with MIDI, so its Snapshot has a chunk table, and waits
// for the first client to have that Snapshot: the Load's reply goes out
// before the worker encodes it, so only its arrival says it was encoded.
// The connect's own Snapshot is waited for first, so it is not taken for
// the Load's.
bool load_midi_session(RealEngine& real, api::Inbox& in) {
    const auto snapshot_from = [&](std::size_t from) {
        for (std::size_t i = from; i < in.model.size(); ++i) {
            if (in.model[i].kind == api::ModelUpdateKind::Snapshot) return true;
        }
        return false;
    };
    if (!until(real, in, [&]() { return snapshot_from(0); })) return false;
    std::string error;
    model::SessionSettings settings;
    settings.name = "Parts";
    model::History h(model::new_session(settings));
    model::AddTrack t;
    t.track = 1;
    t.playlist = 2;
    t.name = "Keys";
    t.playlist_name = "Keys.01";
    t.kind = model::TrackKind::Midi;
    t.channels = 0;
    model::MidiLists seq;
    for (int i = 0; i < 2000; ++i) seq.events.push_back(model::MidiEvent{i * 9600, 0xB0, 1, 2});
    model::MidiRegion r;
    r.id = 3;
    r.name = "Keys_01";
    r.length = 16 * model::kTicksPerQuarter;
    r.data = std::make_shared<const model::MidiSequence>(seq);
    if (!h.apply(t, error) || !h.apply(model::PlaceMidiRegion{2, r, 0, {}}, error)) return false;
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    if (!model::encode_session_bundle(h.current(), model::WriteInfo{"test", 1}, load.op, error)) {
        return false;
    }
    load.op = api::wire::encode_load("", load.op);
    const std::size_t before = in.model.size();
    real.submit(load);
    return until(real, in, [&]() { return snapshot_from(before); });
}

// A client on a transport that never drains, attached straight to the
// server: what it was sent, decoded, and why it was refused.
struct StuckClient {
    struct Got {
        std::mutex mutex;
        std::vector<std::string> frames;
        std::string refused;
    };
    EngineControlServer& server;
    std::shared_ptr<Got> got = std::make_shared<Got>();
    std::shared_ptr<std::atomic<std::size_t>> queued =
        std::make_shared<std::atomic<std::size_t>>(std::size_t{1} << 30);
    EngineControlServer::ClientId id = 0;

    explicit StuckClient(EngineControlServer& s) : server(s) {
        auto g = got;
        auto q = queued;
        id = server.attach(
            [g](EngineControlServer::Frame frame) {
                std::lock_guard<std::mutex> lock(g->mutex);
                g->frames.push_back(*frame);
            },
            [g](const std::string& why) {
                std::lock_guard<std::mutex> lock(g->mutex);
                g->refused = why;
            },
            [q]() { return q->load(); });
    }
    api::Inbox inbox() const {
        std::lock_guard<std::mutex> lock(got->mutex);
        api::wire::Reassembler reassembler;
        api::Inbox in;
        std::string message, why;
        for (const std::string& f : got->frames) {
            if (reassembler.add(f, message, why) == api::wire::Reassembler::Result::Complete) {
                api::wire::decode_into(message, in, why);
            }
        }
        return in;
    }
    std::vector<api::ModelUpdateKind> kinds() const {
        std::vector<api::ModelUpdateKind> out;
        for (const api::ModelUpdate& m : inbox().model) out.push_back(m.kind);
        return out;
    }
    std::string refused() const {
        std::lock_guard<std::mutex> lock(got->mutex);
        return got->refused;
    }
    void send(const api::ModelRequest& m) const {
        std::string bytes, error;
        api::wire::encode(m, bytes);
        std::vector<std::string> frames;
        api::wire::split_frames(bytes, frames, error);
        for (std::string& f : frames) server.deliver(id, std::move(f));
    }
};

std::size_t count(const std::vector<api::ModelUpdateKind>& kinds, api::ModelUpdateKind k) {
    return static_cast<std::size_t>(std::count(kinds.begin(), kinds.end(), k));
}

// ADR-0015 §4: a Snapshot is encoded on the server's worker, and its parts
// go out as the client's transport drains them. A client whose transport is
// full gets its header and waits; an edit meanwhile reaches every other
// client at once and this one only after its window, never inside it.
void snapshot_parts_are_paced_and_held() {
    RealEngine real(fresh_dir("parts"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(load_midi_session(real, in));
    EngineControlServer& server = real.engine().server();
    const uint64_t encoded = server.snapshots_encoded();

    StuckClient stuck(server);
    CHECK(wait_for([&]() { return stuck.kinds().size() == 1; }));
    CHECK(stuck.kinds().front() == api::ModelUpdateKind::SnapshotHeader);
    CHECK(server.snapshots_encoded() == encoded + 1);
    // An edit while the attaching client waits: the first client has it.
    model::AddMarker add;
    add.marker.id = 100;
    add.marker.name = "During";
    api::ModelRequest m;
    CHECK(model::encode_operation(add, m.op, error));
    const std::size_t before = in.model.size();
    real.submit(m);
    CHECK(until(real, in, [&]() { return in.model.size() > before; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(stuck.kinds().size() == 1);
    // The transport drains: the part, the window, then the edit.
    stuck.queued->store(0);
    CHECK(wait_for([&]() { return stuck.kinds().size() == 4; }));
    const std::vector<api::ModelUpdateKind> k = stuck.kinds();
    CHECK(k.size() == 4 && k[1] == api::ModelUpdateKind::SnapshotPart &&
          k[2] == api::ModelUpdateKind::Snapshot && k[3] == api::ModelUpdateKind::Op);
    server.detach(stuck.id);
    std::printf("  the document thread held %llu us at most to take a Snapshot's copy\n",
                static_cast<unsigned long long>(server.longest_snapshot_copy_us()));
}

// Review of #120: a client that stops reading after its Snapshot's header
// used to hold every later broadcast without bound, since its transport
// never filled. Past max_held_bytes it is refused by name, as the socket
// refuses one it cannot queue for, and the other clients go on.
void what_waits_behind_a_snapshot_is_bounded() {
    EngineControlServer::Options opts;
    opts.max_held_bytes = 64 << 10;
    RealEngine real(fresh_dir("held"), opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(load_midi_session(real, in));
    EngineControlServer& server = real.engine().server();
    StuckClient stuck(server);
    CHECK(wait_for([&]() { return stuck.kinds().size() == 1; }));
    const uint64_t refused = server.refused();
    const std::size_t before = in.model.size();
    constexpr int kEdits = 100;
    for (int i = 0; i < kEdits; ++i) {
        model::AddMarker add;
        add.marker.id = 100 + static_cast<model::Id>(i);
        add.marker.position = 48000 * i;
        add.marker.name = std::string(1000, 'x');
        api::ModelRequest m;
        CHECK(model::encode_operation(add, m.op, error));
        real.submit(m);
    }
    CHECK(until(real, in, [&]() { return in.model.size() >= before + kEdits; }));
    CHECK(wait_for([&]() { return !stuck.refused().empty(); }));
    CHECK(stuck.refused().find("stopped reading while its Snapshot was sent") !=
          std::string::npos);
    CHECK(server.refused() == refused + 1);
    CHECK(stuck.kinds().size() == 1);
}

// Review of #120: Resyncs from a client that has not been sent its Snapshot
// share one encoded window instead of each holding one. The first one comes
// while that client's Snapshot is part-sent, so it gets that Snapshot
// again; every one after it is answered by the one already waiting.
void resyncs_share_one_snapshot() {
    RealEngine real(fresh_dir("resyncs"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(load_midi_session(real, in));
    EngineControlServer& server = real.engine().server();
    StuckClient stuck(server);
    CHECK(wait_for([&]() { return stuck.kinds().size() == 1; }));
    const uint64_t encoded = server.snapshots_encoded();
    const uint64_t asked = server.snapshots_asked();
    constexpr int kResyncs = 5;
    for (int i = 0; i < kResyncs; ++i) {
        api::ModelRequest m;
        m.seq = 1000 + static_cast<uint64_t>(i);
        m.action = api::ModelAction::Resync;
        stuck.send(m);
    }
    // Every one handled while the transport is still full.
    CHECK(wait_for([&]() { return server.snapshots_asked() == asked + kResyncs; }));
    CHECK(server.snapshots_encoded() == encoded);
    stuck.queued->store(0);
    CHECK(wait_for([&]() { return stuck.inbox().replies.size() == kResyncs; }));
    const api::Inbox got = stuck.inbox();
    for (const api::CommandReply& r : got.replies) CHECK(r.result == DispatchResult::Ok);
    CHECK(server.snapshots_encoded() == encoded);
    CHECK(count(stuck.kinds(), api::ModelUpdateKind::Snapshot) == 2);
    model::Replica replica;
    std::string why;
    bool fed = true;
    for (const api::ModelUpdate& u : got.model) fed = replica.receive(from_wire(u), why) && fed;
    CHECK(fed && replica.synced());
    server.detach(stuck.id);
}

// Review of #120 (architect): a sequence file over one part, which a real
// session within the budget can hold, opens in parts rather than refused.
void a_sequence_over_a_part_opens_in_parts() {
    RealEngine real(fresh_dir("big_load"), {});
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(until(real, in, [&]() { return !in.model.empty(); }));
    model::SessionSettings settings;
    settings.name = "Big";
    model::History h(model::new_session(settings));
    model::AddTrack t;
    t.track = 1;
    t.playlist = 2;
    t.name = "Synth";
    t.playlist_name = "Synth.01";
    t.kind = model::TrackKind::Midi;
    t.channels = 0;
    model::MidiLists seq;
    std::vector<uint8_t> message(model::kMaxSysexBytes, 0x11);
    message.front() = 0xF0;
    message.back() = 0xF7;
    // Four a chunk: about 18 MiB in one sequence file.
    for (int i = 0; i < 4400; ++i) {
        seq.sysex.push_back(model::MidiSysEx{i * model::kTicksPerQuarter, message});
    }
    model::MidiRegion r;
    r.id = 3;
    r.name = "Synth_01";
    r.length = 16 * model::kTicksPerQuarter;
    r.data = std::make_shared<const model::MidiSequence>(seq);
    CHECK(h.apply(t, error) && h.apply(model::PlaceMidiRegion{2, r, 0, {}}, error));
    std::string load;
    std::vector<std::string> parts;
    CHECK(model::encode_load_parts(h.current(), model::WriteInfo{"test", 1}, load, parts, error,
                                   nullptr, api::wire::kMaxPartBytes));
    CHECK(model::encode_sequence(*r.data).size() > std::size_t{17} << 20 && parts.size() >= 2);
    // Longer than until(): 18 MiB is encoded and decoded on the way.
    const auto polled = [&](const std::function<bool()>& pred) {
        return wait_for([&]() {
            real.poll(in);
            return pred();
        });
    };
    const auto answer = [&](uint64_t seq_no, api::CommandReply& out) {
        return polled([&]() {
            for (const auto& x : in.replies) {
                if (x.seq != seq_no) continue;
                out = x;
                return true;
            }
            return false;
        });
    };
    api::CommandReply reply;
    for (std::string& p : parts) {
        api::ModelRequest m;
        m.action = api::ModelAction::LoadPart;
        m.op = std::move(p);
        CHECK(answer(real.submit(m), reply) && reply.result == DispatchResult::Ok);
    }
    const std::size_t before = in.model.size();
    api::ModelRequest m;
    m.action = api::ModelAction::Load;
    m.op = api::wire::encode_load("", load);
    CHECK(answer(real.submit(m), reply) && reply.result == DispatchResult::Ok);
    model::Replica replica;
    CHECK(polled([&]() {
        return in.model.size() > before && in.model.back().kind == api::ModelUpdateKind::Snapshot;
    }));
    std::string why;
    bool fed = true;
    for (std::size_t i = before; i < in.model.size(); ++i) {
        fed = replica.receive(from_wire(in.model[i]), why) && fed;
    }
    CHECK(fed && replica.synced() &&
          replica.current().edit->tracks[0]->midi_playlists[0]->regions[0].data->lists() ==
              r.data->lists());
}

// ADR-0018 §2, §4 on the real engine: the folder stands from the start, as
// an absolute path, and a client that attaches later is sent the one that
// stands now. A file opened from "Session File Backups/" is of the folder
// above it. A folder another engine holds refuses the Load, and the
// session, its document and its folder stay.
void the_session_folder_follows_the_load() {
    const fs::path dir = fresh_dir("folder");
    const fs::path other = fresh_dir("folder_other");
    const fs::path busy = fresh_dir("folder_busy");
    struct Gone {
        std::vector<fs::path> paths;
        ~Gone() {
            std::error_code ec;
            for (const fs::path& p : paths) fs::remove_all(p, ec);
        }
    } gone{{other, busy}};
    EngineControlServer::Options opts;
    const model::Snapshot start = RealEngine::start_document(dir);
    opts.initial_document = std::make_shared<model::Snapshot>(start);
    RealEngine real(dir, opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto replied = [&](uint64_t seq, DispatchResult want, std::string* message = nullptr) {
        return until(real, in, [&]() {
            for (const auto& r : in.replies) {
                if (r.seq != seq) continue;
                if (message != nullptr) *message = r.message;
                return r.result == want;
            }
            return false;
        });
    };
    const auto folder = [](const api::Inbox& from) -> const api::EngineEvent* {
        for (auto it = from.events.rbegin(); it != from.events.rend(); ++it) {
            if (it->kind == api::EventKind::SessionFolder) return &*it;
        }
        return nullptr;
    };
    const std::string at_start = fs::absolute(dir).lexically_normal().string();
    CHECK(until(real, in, [&]() { return folder(in) != nullptr; }));
    CHECK(folder(in)->text == at_start);
    CHECK((folder(in)->flags & api::kFolderUntitled) == 0);

    std::string bytes;
    CHECK(model::serialize(start, model::WriteInfo{"test", 1}, bytes, error));
    const auto write = [&](const fs::path& file) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::FILE* f = std::fopen(file.string().c_str(), "wb");
        if (f == nullptr) return false;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        return std::fclose(f) == 0 && ok;
    };
    const fs::path backup = other / "Session File Backups" / "Song.v3.sonora";
    CHECK(write(backup));
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    load.op = api::wire::encode_load(backup.string(), bytes);
    const std::size_t before_load = in.model.size();
    CHECK(replied(real.submit(load), DispatchResult::Ok));
    const std::string opened = other.lexically_normal().string();
    CHECK(until(real, in, [&]() { return folder(in) != nullptr && folder(in)->text == opened; }));
    // The Load's Snapshot, encoded on the worker, is in before anything is
    // counted below.
    CHECK(until(real, in, [&]() {
        for (std::size_t i = before_load; i < in.model.size(); ++i) {
            if (in.model[i].kind == api::ModelUpdateKind::Snapshot) return true;
        }
        return false;
    }));
    CHECK(fs::is_directory(other / "Audio Files"));
    CHECK(!fs::exists(other / "Session File Backups" / "Audio Files"));

    // Late: the folder that stands now, not the one at start.
    StuckClient late(real.engine().server());
    late.queued->store(0);
    CHECK(wait_for([&]() {
        const api::Inbox got = late.inbox();
        return folder(got) != nullptr && folder(got)->text == opened;
    }));

    // Another engine holds this one: refused, nothing moves.
    CHECK(write(busy / "Busy.sonora"));
    platform::FolderLock theirs;
    CHECK(theirs.acquire(busy.string(), error) == platform::FolderLock::Result::Held);
    const std::size_t updates = in.model.size();
    const std::size_t announced = std::count_if(in.events.begin(), in.events.end(), [](const auto& e) {
        return e.kind == api::EventKind::SessionFolder;
    });
    load.op = api::wire::encode_load((busy / "Busy.sonora").string(), bytes);
    std::string message;
    CHECK(replied(real.submit(load), DispatchResult::Failed, &message));
    CHECK(message.find("the session's folder could not be opened") != std::string::npos);
    CHECK(message.find("Stop the other engine first") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    real.poll(in);
    CHECK(in.model.size() == updates);
    CHECK(std::count_if(in.events.begin(), in.events.end(), [](const auto& e) {
              return e.kind == api::EventKind::SessionFolder;
          }) == static_cast<std::ptrdiff_t>(announced));
    CHECK(*real.session().folder() == opened);
}

// The folder a slow save writes, and whether its lock was free while it
// did: Options::save_sync_directory, called on the saver's thread.
std::mutex slow_save_mutex;
std::string slow_save_folder;
std::atomic<int> slow_save_syncs{0};
std::atomic<bool> slow_save_unlocked{false};

bool slow_save_sync(const std::string&) {
    if (slow_save_syncs.fetch_add(1) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::lock_guard<std::mutex> lock(slow_save_mutex);
    platform::FolderLock probe;
    std::string error;
    if (probe.acquire(slow_save_folder, error) == platform::FolderLock::Result::Held) {
        slow_save_unlocked.store(true);
    }
    return true;
}

// PR #142 review N3: a Load did not wait for the saver. A Save sent before
// it was still writing the old folder after the Load had let go of its
// lock, so another engine could have taken the folder in the meantime; and
// the Save's reply came after the Load's.
void a_load_waits_for_the_save_before_it() {
    const fs::path dir = fresh_dir("fence");
    const fs::path first = fresh_dir("fence_first");
    const fs::path second = fresh_dir("fence_second");
    struct Gone {
        std::vector<fs::path> paths;
        ~Gone() {
            std::error_code ec;
            for (const fs::path& p : paths) fs::remove_all(p, ec);
        }
    } gone{{first, second}};
    EngineControlServer::Options opts;
    const model::Snapshot start = RealEngine::start_document(dir);
    opts.initial_document = std::make_shared<model::Snapshot>(start);
    opts.save_sync_directory = slow_save_sync;
    RealEngine real(dir, opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto reply_at = [&](uint64_t seq) -> std::ptrdiff_t {
        for (std::size_t i = 0; i < in.replies.size(); ++i) {
            if (in.replies[i].seq == seq) return static_cast<std::ptrdiff_t>(i);
        }
        return -1;
    };
    std::string bytes;
    CHECK(model::serialize(start, model::WriteInfo{"test", 1}, bytes, error));
    const auto write = [&](const fs::path& file) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::FILE* f = std::fopen(file.string().c_str(), "wb");
        if (f == nullptr) return false;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        return std::fclose(f) == 0 && ok;
    };
    const fs::path first_file = first / "First.sonora";
    CHECK(write(first_file));
    CHECK(write(second / "Second.sonora"));
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    load.op = api::wire::encode_load(first_file.string(), bytes);
    const uint64_t opened = real.submit(load);
    CHECK(until(real, in, [&]() { return reply_at(opened) >= 0; }));
    CHECK(in.replies[static_cast<std::size_t>(reply_at(opened))].result == DispatchResult::Ok);

    {
        std::lock_guard<std::mutex> lock(slow_save_mutex);
        slow_save_folder = first.lexically_normal().string();
    }
    api::ModelRequest save;
    save.action = api::ModelAction::Save;
    save.op = api::wire::encode_save(first_file.string(), bytes);
    const uint64_t saved = real.submit(save);
    load.op = api::wire::encode_load((second / "Second.sonora").string(), bytes);
    const uint64_t moved = real.submit(load);
    CHECK(until(real, in, [&]() { return reply_at(saved) >= 0 && reply_at(moved) >= 0; }));
    CHECK(reply_at(saved) >= 0 &&
          in.replies[static_cast<std::size_t>(reply_at(saved))].result == DispatchResult::Ok);
    CHECK(reply_at(moved) >= 0 &&
          in.replies[static_cast<std::size_t>(reply_at(moved))].result == DispatchResult::Ok);
    CHECK(slow_save_syncs.load() > 0);
    CHECK(!slow_save_unlocked.load());
    CHECK(reply_at(saved) < reply_at(moved));
    CHECK(*real.session().folder() == second.lexically_normal().string());
    // PR #147 review N-A: the control thread waits for the save, so the
    // operator is told why the Load is not answered yet.
    CHECK(std::any_of(in.events.begin(), in.events.end(), [](const api::EngineEvent& e) {
        return e.kind == api::EventKind::Notice &&
               e.text.find("Finishing the save before opening") != std::string::npos;
    }));
}

// The last SessionFolder event `in` holds, or null.
const api::EngineEvent* last_folder(const api::Inbox& in) {
    for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
        if (it->kind == api::EventKind::SessionFolder) return &*it;
    }
    return nullptr;
}

bool touch(const fs::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) return false;
    return std::fclose(f) == 0;
}

// ADR-0018 §3: a Load with an empty path is a new session, and gets a
// folder of its own under the sessions root; until this item it kept the
// folder of the session before it, so two sessions shared one. A fresh
// Untitled folder, with nothing in it yet, is the new session's as it is.
void a_new_session_gets_its_own_untitled_folder() {
    const fs::path root = fresh_dir("untitled_root");
    struct Gone {
        fs::path p;
        ~Gone() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } gone{root};
    const fs::path first = root / "Untitled";
    fs::create_directories(first);
    EngineControlServer::Options opts;
    opts.sessions_root = root.string();
    const model::Snapshot start = RealEngine::start_document(first);
    opts.initial_document = std::make_shared<model::Snapshot>(start);
    RealEngine real(first, opts, false, true);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(until(real, in, [&]() { return last_folder(in) != nullptr; }));
    CHECK(last_folder(in) != nullptr && last_folder(in)->text == first.string() &&
          (last_folder(in)->flags & api::kFolderUntitled) != 0);

    std::string bytes;
    CHECK(model::serialize(start, model::WriteInfo{"test", 1}, bytes, error));
    api::ModelRequest load;
    load.action = api::ModelAction::Load;
    load.op = api::wire::encode_load("", bytes);
    const auto loaded = [&](uint64_t seq) {
        return until(real, in, [&]() {
            return std::any_of(in.replies.begin(), in.replies.end(), [&](const api::CommandReply& r) {
                return r.seq == seq && r.result == DispatchResult::Ok;
            });
        });
    };
    // Nothing is in it yet: the new session takes it.
    CHECK(loaded(real.submit(load)));
    CHECK(*real.session().folder() == first.string());
    CHECK(!fs::exists(root / "Untitled 2"));

    CHECK(touch(first / "Audio Files" / "notes.txt"));
    CHECK(loaded(real.submit(load)));
    const std::string second = (root / "Untitled 2").string();
    CHECK(until(real, in, [&]() { return last_folder(in) != nullptr && last_folder(in)->text == second; }));
    CHECK(last_folder(in) != nullptr && (last_folder(in)->flags & api::kFolderUntitled) != 0);
    CHECK(*real.session().folder() == second);
    CHECK(fs::exists(first / "Audio Files" / "notes.txt"));
}

// ADR-0018 §3 and PR #142 review N2: the first Save of an untitled session
// renames its folder to the one the interface names, so the takes stay
// beside the session file and a session called "Bass" is saved as Bass,
// not as its folder's "Untitled".
void the_first_save_names_the_untitled_folder() {
    const fs::path root = fresh_dir("first_save_root");
    struct Gone {
        fs::path p;
        ~Gone() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } gone{root};
    const fs::path untitled = root / "Untitled";
    fs::create_directories(untitled);
    EngineControlServer::Options opts;
    opts.sessions_root = root.string();
    model::Snapshot start = RealEngine::start_document(untitled);
    start.settings.name = "Bass";
    opts.initial_document = std::make_shared<model::Snapshot>(start);
    RealEngine real(untitled, opts, false, true);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    CHECK(touch(untitled / "Audio Files" / "notes.txt"));
    std::string bytes;
    CHECK(model::serialize(start, model::WriteInfo{"test", 1}, bytes, error));
    const fs::path file = root / "Bass" / "Bass.sonora";
    api::ModelRequest save;
    save.action = api::ModelAction::Save;
    save.op = api::wire::encode_save(file.string(), bytes);
    const auto saved = [&](uint64_t seq, std::string* message = nullptr) {
        api::CommandReply got;
        const bool answered = until(real, in, [&]() {
            for (const api::CommandReply& r : in.replies)
                if (r.seq == seq) got = r;
            return got.seq == seq;
        });
        if (message != nullptr) *message = got.message;
        return answered && got.result == DispatchResult::Ok;
    };
    std::string message;
    CHECK(saved(real.submit(save), &message));
    if (!message.empty()) std::printf("  save: %s\n", message.c_str());
    const std::string named = (root / "Bass").string();
    CHECK(until(real, in, [&]() { return last_folder(in) != nullptr && last_folder(in)->text == named; }));
    CHECK(last_folder(in) != nullptr && (last_folder(in)->flags & api::kFolderUntitled) == 0);
    CHECK(*real.session().folder() == named);
    CHECK(!fs::exists(untitled));
    CHECK(fs::is_regular_file(file));
    CHECK(fs::exists(root / "Bass" / "Audio Files" / "notes.txt"));
    platform::FolderLock probe;
    CHECK(probe.acquire(named, error) == platform::FolderLock::Result::Busy);

    // Named now: the next Save writes in place.
    const std::size_t announced = std::count_if(in.events.begin(), in.events.end(), [](const auto& e) {
        return e.kind == api::EventKind::SessionFolder;
    });
    CHECK(saved(real.submit(save)));
    CHECK(std::count_if(in.events.begin(), in.events.end(), [](const auto& e) {
              return e.kind == api::EventKind::SessionFolder;
          }) == static_cast<std::ptrdiff_t>(announced));
    CHECK(*real.session().folder() == named);
}

// The round trip kept per machine (ADR-0019, update of 2026-10-26): the
// source the start chose stands for a late client too, and a measured or
// manual L is written to the machine's file under the server's key; the
// file is never touched without one.
void the_round_trip_is_kept_for_the_machine() {
    const fs::path conf_dir = fresh_dir("latency_conf");
    const std::string file = machine_latency_file(conf_dir.string());
    EngineControlServer::Options opts;
    opts.latency_source = api::LatencySource::Backend;
    opts.latency_file = file;
    opts.latency_key = {"UMC1820 Pro", 48000, 128};
    RealEngine real(fresh_dir("latency"), opts);
    std::string error;
    CHECK(real.connect(error));
    api::Inbox in;
    const auto latest = [&]() -> const api::EngineEvent* {
        for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
            if (it->kind == api::EventKind::Latency) return &*it;
        }
        return nullptr;
    };
    CHECK(until(real, in, [&]() { return latest() != nullptr; }));
    CHECK(latest() && api::latency_source(latest()->flags) == api::LatencySource::Backend);
    KeptLatency kept;
    CHECK(!load_machine_latency(file, opts.latency_key, kept));

    api::CommandRequest set;
    set.command = "latency.set";
    set.argument = "250";
    const uint64_t seq = real.send(set);
    CHECK(until(real, in, [&]() {
        for (const auto& r : in.replies) {
            if (r.seq == seq) return r.result == DispatchResult::Ok;
        }
        return false;
    }));
    CHECK(real.session().input_latency() == 250);
    // latency.set writes the file before it answers.
    CHECK(load_machine_latency(file, opts.latency_key, kept) && kept.frames == 250 &&
          kept.source == api::LatencySource::Manual);

    api::CommandRequest measure;
    measure.command = "latency.measure";
    measure.argument = "2:" + std::to_string(kTracks);
    real.send(measure);
    // About 1.5 s of real time: three pings of two quarter-second windows.
    const auto end = Clock::now() + std::chrono::seconds(6);
    while (Clock::now() < end) {
        real.poll(in);
        if (latest() && api::latency_status(latest()->flags) != api::LatencyStatus::Measuring &&
            api::latency_status(latest()->flags) != api::LatencyStatus::Idle) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(latest() && api::latency_status(latest()->flags) == api::LatencyStatus::Measured);
    CHECK(real.session().input_latency() == RealEngine::kCableFrames);
    // Written on the control thread just after the event is sent.
    CHECK(until(real, in, [&]() {
        return load_machine_latency(file, opts.latency_key, kept) &&
               kept.frames == RealEngine::kCableFrames &&
               kept.source == api::LatencySource::Measured;
    }));
    // Every ping was said while it ran.
    std::size_t progress = 0;
    for (const auto& e : in.events) {
        if (e.kind == api::EventKind::Latency &&
            api::latency_status(e.flags) == api::LatencyStatus::Measuring) {
            ++progress;
        }
    }
    CHECK(progress >= 3);

    // A client attaching now is greeted with L as it stands, once.
    const std::vector<api::EngineEvent> late = late_greeting(real, in);
    std::size_t latency_events = 0;
    for (const auto& e : late) {
        if (e.kind != api::EventKind::Latency) continue;
        ++latency_events;
        CHECK(e.frame == RealEngine::kCableFrames &&
              api::latency_source(e.flags) == api::LatencySource::Measured && e.insert == 2);
    }
    CHECK(latency_events == 1);
    std::error_code ec;
    fs::remove_all(conf_dir, ec);
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
    snapshot_parts_are_paced_and_held();
    what_waits_behind_a_snapshot_is_bounded();
    resyncs_share_one_snapshot();
    a_sequence_over_a_part_opens_in_parts();
    a_save_during_a_take_follows_a_checkpoint();
    recovery_offer_at_start_and_after_a_take();
    a_take_just_punched_out_is_never_offered();
    the_session_folder_follows_the_load();
    a_load_waits_for_the_save_before_it();
    a_new_session_gets_its_own_untitled_folder();
    the_first_save_names_the_untitled_folder();
    the_round_trip_is_kept_for_the_machine();
    if (failures) return EXIT_FAILURE;
    std::printf("control: ok\n");
    return EXIT_SUCCESS;
}
