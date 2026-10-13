// SPDX-License-Identifier: GPL-3.0-or-later
//
// Record, then play back, end to end against drivers/fake (ADR-0007 §7,
// ADR-0009 §4.6), through the control server as an interface drives it
// (ADR-0009, update "playback on the M4/M7 control server"): track 1
// records the fake's channel-0 tone for a second; the take joins the
// server's document, and every client is sent the AddSource and RecordTake;
// return to zero and play go through the residency gate; and the fake's
// captured outputs must be the recorded tone at the -3 dB centre gain on
// both sides of the monitor bus, from the first frame rolled, with every
// other block written and no chunk missing. Then the record reply's three
// outcomes: Ok once the take has started, Failed with the reason when it is
// refused, Failed "cancelled" when a second press withdraws it.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/session.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::FakeBackend;
using sonora::DispatchResult;
using sonora::TransportState;

constexpr uint32_t kRate = 48000;
constexpr uint32_t kQuantum = 128;

std::size_t drain(FakeBackend& audio, uint32_t index, std::vector<float>* into = nullptr) {
    float buf[4096];
    std::size_t total = 0;
    for (;;) {
        const std::size_t n = audio.read_output(index, buf, 4096);
        if (into) into->insert(into->end(), buf, buf + n);
        total += n;
        if (n < 4096) return total;
    }
}

// A client of the server, as an interface's tick is: everything that
// arrives is kept.
struct Client {
    sonora::InProcessEngine& engine;
    sonora::api::Inbox inbox;

    uint64_t send(const std::string& id, int32_t target = -1) {
        sonora::api::CommandRequest r;
        r.command = id;
        r.target = target;
        return engine.send(r);
    }
    const sonora::api::CommandReply* reply(uint64_t seq) {
        engine.poll(inbox);
        for (const auto& r : inbox.replies) {
            if (r.seq == seq) return &r;
        }
        return nullptr;
    }
    // Waits up to five seconds; the fake runs no blocks meanwhile.
    bool await(uint64_t seq, sonora::api::CommandReply& out) {
        for (int i = 0; i < 2500; ++i) {
            if (const auto* r = reply(seq)) {
                out = *r;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    bool run(const std::string& id, int32_t target = -1) {
        sonora::api::CommandReply r;
        if (!await(send(id, target), r)) return false;
        if (r.result != DispatchResult::Ok) {
            std::fprintf(stderr, "  %s: %s\n", id.c_str(), r.message.c_str());
        }
        return r.result == DispatchResult::Ok;
    }
};

sonora::SessionConfig config(const fs::path& dir) {
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.sample_rate = kRate;
    cfg.format = sonora::WavWriter::Format::Float32;
    cfg.ring_seconds = 2.0;
    cfg.playback.chunk_frames = 4096;
    cfg.playback.window_seconds = 0.1;
    cfg.playback.max_block_frames = 256;
    return cfg;
}

int test_record_then_play_back() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());

    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    fc.output_capture_frames = 4 * kRate;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();

    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};

        // One second of channel 0's tone on track 1, from server frame 0:
        // the reply comes once the take has started, before any block.
        CHECK(c.run("track.toggle_arm", 0));
        CHECK(c.run("transport.record"));
        CHECK(s.transport().state() == TransportState::Recording);
        constexpr uint32_t kBlocks = kRate / kQuantum;
        CHECK(audio.run_blocks(kBlocks) == kBlocks);
        CHECK(c.run("transport.stop"));
        CHECK(s.tracks()[0]->writer().frames_written() == uint64_t{kBlocks} * kQuantum);
        // After the stop: the take's tracks are the ones armed when it closed.
        CHECK(c.run("track.toggle_arm", 0));

        // The take is in the document every client replicates: its source
        // and its region, one update each.
        std::size_t ops = 0;
        for (int i = 0; i < 500 && ops < 2; ++i) {
            engine.poll(c.inbox);
            ops = 0;
            for (const auto& m : c.inbox.model) {
                ops += m.kind == sonora::api::ModelUpdateKind::Op ? 1 : 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(ops == 2);

        CHECK(c.run("transport.return_to_zero"));
        CHECK(c.run("transport.play"));
        drain(audio, 0);
        drain(audio, 1);
        // The callback keeps running while the gate waits: the locate lands
        // at a block start, and the roll starts once the take's window is
        // resident. Every frame from here is kept, so the rolled ones are
        // the last `playhead` of them.
        std::vector<float> left, right;
        for (int i = 0; i < 2000 && s.transport().state() != TransportState::Rolling; ++i) {
            CHECK(audio.run_blocks(1) == 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(s.transport().state() == TransportState::Rolling);
        CHECK(s.transport().playhead() <= kQuantum);

        // The whole take and a little past its end, at the pace the
        // reader's thread keeps up with.
        constexpr uint32_t kPlayBlocks = kBlocks + 20;
        for (uint32_t b = 0; b < kPlayBlocks; ++b) {
            CHECK(audio.run_blocks(1) == 1);
            if (b % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drain(audio, 0, &left);
        drain(audio, 1, &right);
        CHECK(right.size() == left.size());
        const uint64_t rolled = s.transport().playhead();
        CHECK(rolled >= uint64_t{kPlayBlocks} * kQuantum);
        CHECK(left.size() >= rolled);
        const std::size_t first = left.size() - static_cast<std::size_t>(rolled);

        const float gain = std::pow(10.0f, -3.0f / 20.0f);
        const uint64_t take = uint64_t{kBlocks} * kQuantum;
        double worst = 0.0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            const uint64_t t = i - first;
            const float want =
                i < first ? 0.0f : (t < take ? FakeBackend::tone(0, t, kRate) * gain : 0.0f);
            worst = std::max(worst, static_cast<double>(std::fabs(left[i] - want)));
            worst = std::max(worst, static_cast<double>(std::fabs(right[i] - want)));
        }
        std::printf("playback through the server on drivers/fake: %llu frames rolled,"
                    " worst residual %.3g\n",
                    static_cast<unsigned long long>(rolled), worst);
        CHECK(worst < 1e-6);
        CHECK(s.playback_misses() == 0);
        CHECK(audio.unwritten_output_periods() == 0);
        CHECK(audio.output_dropped() == 0);
        CHECK(audio.xruns() == 0);
        CHECK(c.run("transport.stop"));

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

// What a record reply says once the gate resolves. The fake runs no
// blocks, so a take asked for behind a locate waits: the locate is applied
// only at a block start.
int test_record_replies() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake_replies";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());
    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();
    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};
        sonora::api::CommandReply r;

        // Refused inside the gate: the reason, not an Ok.
        CHECK(c.await(c.send("transport.record"), r));
        CHECK(r.result == DispatchResult::Failed);
        CHECK(r.message.find("no track is armed") != std::string::npos);

        // Withdrawn: the first press waits behind the locate, and the
        // second takes it back. The first is told so; the second is Ok.
        CHECK(c.run("track.toggle_arm", 1));
        CHECK(c.run("transport.return_to_zero"));
        const uint64_t first = c.send("transport.toggle_record");
        for (int i = 0; i < 25; ++i) {
            CHECK(c.reply(first) == nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(s.transport().state() == TransportState::Stopped);
        CHECK(c.run("transport.toggle_record"));
        CHECK(c.await(first, r));
        CHECK(r.result == DispatchResult::Failed && r.message == "cancelled");
        CHECK(s.transport().state() == TransportState::Stopped);

        // Started: held until the locate lands and the take opens, then Ok.
        const uint64_t again = c.send("transport.record");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(c.reply(again) == nullptr);
        CHECK(audio.run_blocks(1) == 1);
        CHECK(c.await(again, r));
        CHECK(r.result == DispatchResult::Ok);
        CHECK(s.transport().state() == TransportState::Recording);
        CHECK(audio.run_blocks(4) == 4);
        CHECK(c.run("transport.stop"));

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    if (test_record_then_play_back()) return 1;
    if (test_record_replies()) return 1;
    std::printf("playback_fake: ok\n");
    return 0;
}
