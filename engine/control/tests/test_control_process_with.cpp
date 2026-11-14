// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process Selection With through the real control server (ADR-0087): the
// wire's insert=<id> reaching RenderInPlace with the hosting's render-scoped
// solo set, on drivers/fake.
//
//   (no arguments)       Hello announces track.render.insert; freeze and
//                        commit refuse insert=; process_selection reads it
//                        and hands it to plan_render (an insert the track
//                        does not hold is refused by name, a held one gets
//                        as far as the hosting check)
//   --host H --bundle B  the same request against real sonora-plugin-host
//                        processes: two eg-amps, the first at -90 dB (off),
//                        the second at 0 dB. Processed through the second
//                        alone, the file holds the take; through both it
//                        would be silence. Exit 77 without eg-amp.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/render.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

using namespace sonora;
using namespace sonora::model;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr int kSkip = 77;
constexpr uint32_t kBlock = 128;
constexpr int64_t kFrames = 48000;
constexpr float kLevel = 0.25f;
const char* const kEgAmp = "http://lv2plug.in/plugins/eg-amp";
constexpr Id kTrack = 100;
constexpr Id kOff = 500;    // eg-amp at -90 dB: silence when it plays
constexpr Id kUnity = 501;  // eg-amp at 0 dB

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

struct Args {
    std::string host;
    std::vector<std::string> bundles;
};

// Track A with the two eg-amps and one region of a constant take.
std::shared_ptr<Snapshot> document_with_take(const std::string& dir) {
    {
        WavWriter w;
        w.open(dir + "/Audio Files/take.wav", 48000, 1, WavWriter::Format::Float32, 0);
        const std::vector<float> data(kFrames, kLevel);
        w.write(data.data(), data.size());
        w.close();
    }
    History h(new_session(SessionSettings{}));
    std::string e;
    AddTrack t;
    t.track = kTrack;
    t.playlist = kTrack + 1;
    t.name = "A";
    t.playlist_name = "A.01";
    t.mix.input.push_back(0);
    h.apply(t, e);
    const auto amp = [](Id id, uint8_t slot, float gain_db) {
        AddInsert op;
        op.track = kTrack;
        op.insert = id;
        op.slot = slot;
        op.plugin = PluginRef{PluginFormat::Lv2, kEgAmp, kEgAmp, "test"};
        op.params = {{"gain", gain_db}};
        return op;
    };
    h.apply(amp(kOff, 0, -90.f), e);
    h.apply(amp(kUnity, 1, 0.f), e);
    AddSource add;
    add.source.id = h.current().next_id;
    add.source.path = "Audio Files/take.wav";
    add.source.channels = 1;
    add.source.length = kFrames;
    add.source.format = AudioFormat::Float32;
    h.apply(add, e);
    PlaceRegion place;
    place.playlist = kTrack + 1;
    place.region.id = h.current().next_id;
    place.region.name = "take";
    place.region.position = 0;
    place.region.length = kFrames;
    place.region.channels.push_back({add.source.id, 0});
    place.split_right_id = place.region.id + 100;
    if (!h.apply(place, e)) std::printf("  rig: %s\n", e.c_str());
    return std::make_shared<Snapshot>(h.current());
}

// A session on drivers/fake and the server over it, owned together.
class Rig {
public:
    Rig(const std::string& dir, const Args& a) : dir_(dir), session_(config(dir)), audio_(backend()) {
        std::filesystem::remove_all(dir_);
        session_.create_dirs();
        std::string error;
        ok_ = audio_.open({"sonora", 2, 2, 48000, ""}, session_, error) && audio_.activate(error);
        if (!ok_) std::printf("  rig: %s\n", error.c_str());
        session_.disk().start();
        EngineControlServer::Options opts;
        opts.plugin_host = a.host;
        opts.plugin_bundles = a.bundles;
        opts.plugin_scan_at_start = false;
        opts.initial_document = document_with_take(dir_);
        engine_ = std::make_unique<InProcessEngine>(session_, &audio_, opts);
        ok_ = ok_ && engine_->connect(error);
    }
    ~Rig() {
        engine_->shutdown();
        audio_.deactivate();
        engine_->server().after_deactivate();
        session_.stop();
        engine_.reset();
        std::filesystem::remove_all(dir_);
    }

    bool ok() const { return ok_; }
    InProcessEngine& engine() { return *engine_; }
    api::Inbox& inbox() { return in_; }
    const std::string& dir() const { return dir_; }

    // No wall-clock bound: the reply ends the wait, a hang is ctest's timeout.
    api::CommandReply run(const char* command, const std::string& arg) {
        api::CommandRequest c;
        c.command = command;
        c.argument = arg;
        const uint64_t seq = engine_->send(c);
        for (;;) {
            engine_->poll(in_);
            for (const api::CommandReply& r : in_.replies) {
                if (r.seq == seq) return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    bool until(const std::function<bool()>& pred, uint64_t max_ms) {
        const uint64_t end = now_ms() + max_ms;
        while (now_ms() < end) {
            engine_->poll(in_);
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        engine_->poll(in_);
        return pred();
    }

private:
    static SessionConfig config(const std::string& dir) {
        SessionConfig cfg;
        cfg.dir = dir;
        cfg.track_count = 2;
        cfg.ring_seconds = 0.5;
        cfg.playback.max_param_stores = 16;
        return cfg;
    }
    static FakeBackend::Config backend() {
        FakeBackend::Config bc;
        bc.devices = {FakeBackend::onboard()};
        bc.mode = FakeBackend::Mode::Thread;
        return bc;
    }

    std::string dir_;
    Session session_;
    FakeBackend audio_;
    std::unique_ptr<InProcessEngine> engine_;
    api::Inbox in_;
    bool ok_ = false;
};

std::string ask(Id insert) {
    api::RenderOptions o;
    o.track = kTrack;
    o.start = 4800;
    o.end = 28800;
    o.only_insert = insert;
    return api::format_render_argument(o);
}

bool contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

int hostless() {
    std::printf("hostless: the wire's insert= through the server's parse and plan\n");
    Rig rig("test_control_process_with_hostless", Args{});
    check(rig.ok(), "rig up");
    const auto& caps = rig.engine().hello().capabilities;
    check(std::find(caps.begin(), caps.end(), api::kCapabilityRenderInsert) != caps.end(),
          "Hello announces track.render.insert");

    for (const char* command : {"track.freeze", "track.commit"}) {
        const api::CommandReply r = rig.run(command, ask(kUnity));
        check(r.result == DispatchResult::Failed && contains(r.message, "applies only to Process Selection"),
              "freeze and commit refuse insert= by its rule");
    }
    const api::CommandReply unknown = rig.run(api::kCommandProcessSelection, ask(999));
    check(unknown.result == DispatchResult::Failed && contains(unknown.message, "not an active insert of A"),
          "process_selection hands insert= to the plan: an insert A lacks is refused");
    const api::CommandReply held = rig.run(api::kCommandProcessSelection, ask(kUnity));
    check(held.result == DispatchResult::Failed && contains(held.message, "plug-in hosting is not running"),
          "  one A holds passes the plan and stops only at the hosting");
    for (const api::EngineEvent& e : rig.inbox().events) {
        if (e.kind == api::EventKind::Render) {
            check(false, "no Render event for a refused request");
            break;
        }
    }
    return failures ? 1 : 0;
}

float peak_of(const std::string& path) {
    WavReader rd;
    std::string error;
    if (!rd.open(path, error)) return -1.f;
    std::vector<float> d(static_cast<std::size_t>(rd.frames()) * rd.channels());
    const int64_t got = rd.read(0, rd.frames(), d.data());
    rd.close();
    float peak = 0.f;
    for (int64_t i = 0; i < got * rd.channels(); ++i) peak = std::max(peak, std::fabs(d[std::size_t(i)]));
    return peak;
}

int with_host(const Args& a) {
    std::printf("with_host: process_selection insert=%llu on real hosts\n",
                static_cast<unsigned long long>(kUnity));
    if (!std::filesystem::exists("/usr/lib/lv2/eg-amp.lv2")) {
        std::printf("  skip: eg-amp (lv2-examples) is not installed\n");
        return kSkip;
    }
    Rig rig("test_control_process_with_host", a);
    check(rig.ok(), "rig up");
    const auto running = [&](int slot) {
        return (rig.engine().state().track(0).inserts[slot].flags & api::kInsertRunning) != 0;
    };
    check(rig.until([&] { return running(0) && running(1); }, 30000), "both eg-amps running");
    check(rig.until([&] { return rig.engine().state().track(0).latency_frames == kBlock; }, 10000),
          "the playing plan binds the chain");

    api::CommandReply r;
    // The plan that binds the chain can still be settling: stopped, but busy.
    rig.until(
        [&] {
            r = rig.run(api::kCommandProcessSelection, ask(kUnity));
            return r.result == DispatchResult::Ok || !contains(r.message, "stop the transport");
        },
        5000);
    if (r.result != DispatchResult::Ok) std::printf("  refused: %s\n", r.message.c_str());
    check(r.result == DispatchResult::Ok, "process_selection insert= starts the render");

    api::EngineEvent end;
    const auto ended = [&] {
        for (const api::EngineEvent& e : rig.inbox().events) {
            if (e.kind == api::EventKind::Render &&
                e.flags != static_cast<uint32_t>(api::RenderState::Running)) {
                end = e;
                return true;
            }
        }
        return false;
    };
    // The roll is real time: preroll, the range, and the solo's confirmation.
    check(rig.until(ended, 30000), "the render ended");
    if (end.flags != static_cast<uint32_t>(api::RenderState::Done)) std::printf("  ended: %s\n", end.text.c_str());
    check(end.flags == static_cast<uint32_t>(api::RenderState::Done) && end.insert == kTrack, "  Done, for A");

    std::string printed;
    for (const auto& f : std::filesystem::directory_iterator(rig.dir() + "/Audio Files")) {
        if (f.path().extension() == ".wav" && f.path().filename() != "take.wav") printed = f.path().string();
    }
    check(!printed.empty(), "the print is in Audio Files");
    const float peak = peak_of(printed);
    std::printf("  peak %.4f (the take is %.2f)\n", static_cast<double>(peak), static_cast<double>(kLevel));
    check(std::fabs(peak - kLevel) < 0.01f, "the print went through the 0 dB eg-amp alone, not the -90 dB one");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--host") a.host = argv[i + 1];
        else if (k == "--bundle") a.bundles.push_back(argv[i + 1]);
    }
    const int rc = a.host.empty() ? hostless() : with_host(a);
    if (rc == kSkip) return kSkip;
    std::printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
