// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/test-engine-docker.sh's PLAYBACK check in process: track 1's take
// plays on the monitor bus while track 2 records out_1, looped back into
// in_2 two periods late, and the second take must be the first at the pan
// law's centre gain, two periods late, at every frame. The periods are
// paced as a server would, with jitter and stalls, while the disk reader
// and the control server's thread run on threads of their own, as they do
// in sonora-engine, and the take is driven through the server as
// sonora-engine's headless --record drives it.
//
// The docker check failed about 1 run in 30, always with two adjacent
// blocks of the recorded take swapped (2026-10-13). The engine's out_1, as
// it left the callback, was continuous in every failure; PipeWire had
// skipped a period, and its JACK layer gives a self-linked input the
// output's own buffer, alternating between two, so in_2 read a buffer one
// period early and then one three periods late. This test holds the
// engine's side of that: nothing here may move a block, whatever the
// threads' timing. SONORA_LOOPBACK_ROUNDS raises the number of takes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

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

constexpr uint32_t kRate = 48000;
constexpr uint32_t kQuantum = 128;
constexpr uint32_t kLoopPeriods = 2;  // what the docker check's loop measured
constexpr uint32_t kSourceBlocks = kRate / kQuantum / 2;  // half a second
constexpr uint64_t kTakeFrames = uint64_t{kRate} * 3 / 4;

bool read_mono(const fs::path& path, std::vector<float>& out) {
    sonora::WavReader r;
    std::string error;
    if (!r.open(path.string(), error) || r.channels() != 1) return false;
    out.assign(static_cast<std::size_t>(r.frames()), 0.0f);
    return r.read(0, r.frames(), out.data()) == r.frames();
}

// The audio thread: a period every 2.67 ms, late by up to a millisecond,
// and now and then not at all for tens of milliseconds, after which the
// schedule restarts rather than catching up, as PipeWire's dummy driver
// does. in_2 is out_1 from kLoopPeriods periods before.
class LoopDriver {
public:
    LoopDriver(sonora::Session& s, uint32_t seed) : s_(s), rng_(seed) {
        for (auto& b : loop_) b.assign(kQuantum, 0.0f);
        out_[0].assign(kQuantum, 0.0f);
        out_[1].assign(kQuantum, 0.0f);
        thread_ = std::thread([this] { run(); });
    }
    ~LoopDriver() {
        run_.store(false);
        thread_.join();
    }

private:
    void run() {
        const auto period = std::chrono::nanoseconds(1000000000ull * kQuantum / kRate);
        std::uniform_int_distribution<int> late_us(0, 1000);
        std::uniform_int_distribution<int> stall(0, 199);
        std::uniform_int_distribution<int> stall_ms(5, 40);
        auto next = std::chrono::steady_clock::now();
        uint64_t n = 0;
        while (run_.load()) {
            const float* in[2] = {nullptr, loop_[n % (kLoopPeriods + 1)].data()};
            float* out[2] = {out_[0].data(), out_[1].data()};
            sonora::backend::AudioBlock block{};
            block.inputs = in;
            block.input_count = 2;
            block.outputs = out;
            block.output_count = 2;
            block.nframes = kQuantum;
            block.frame_time = n * kQuantum;
            s_.process(block);
            // Read again kLoopPeriods periods from now.
            std::copy(out_[0].begin(), out_[0].end(),
                      loop_[(n + kLoopPeriods) % (kLoopPeriods + 1)].begin());
            ++n;
            next += period + std::chrono::microseconds(late_us(rng_));
            if (stall(rng_) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms(rng_)));
                next = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_until(next);
        }
    }

    sonora::Session& s_;
    std::mt19937 rng_;
    std::vector<float> loop_[kLoopPeriods + 1];
    std::vector<float> out_[2];
    std::atomic<bool> run_{true};
    std::thread thread_;
};

// The reply to a command, waiting up to ten seconds: a take waits for its
// window.
bool run(sonora::InProcessEngine& engine, const std::string& id, int32_t target = -1) {
    sonora::api::CommandRequest r;
    r.command = id;
    r.target = target;
    const uint64_t seq = engine.send(r);
    sonora::api::Inbox in;
    for (int i = 0; i < 5000; ++i) {
        engine.poll(in);
        for (const auto& reply : in.replies) {
            if (reply.seq != seq) continue;
            if (reply.result != sonora::DispatchResult::Ok) {
                std::fprintf(stderr, "  %s: %s\n", id.c_str(), reply.message.c_str());
            }
            return reply.result == sonora::DispatchResult::Ok;
        }
        in.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::fprintf(stderr, "  %s: no reply\n", id.c_str());
    return false;
}

int loopback_round(uint32_t seed) {
    const fs::path dir = fs::temp_directory_path() /
                         ("sonora_test_playback_loopback_" + std::to_string(seed));
    fs::remove_all(dir);
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.sample_rate = kRate;
    cfg.format = sonora::WavWriter::Format::Float32;
    cfg.ring_seconds = 2.0;
    cfg.playback.chunk_frames = 4096;
    cfg.playback.window_seconds = 0.1;
    cfg.playback.max_block_frames = 256;
    sonora::Session s(cfg);
    CHECK(s.create_dirs());
    std::string error;

    // Track 1's take: the fake's channel-0 tone.
    {
        FakeBackend::Config fc;
        fc.quantum = kQuantum;
        FakeBackend audio(fc);
        CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
        CHECK(audio.activate(error));
        CHECK(audio.connect_inputs_in_order() == 2);
        s.disk().start();
        s.tracks()[0]->set_rec_armed(true);
        CHECK(s.start_recording(1, &error));
        CHECK(audio.run_blocks(kSourceBlocks) == kSourceBlocks);
        CHECK(s.stop(&error));
        audio.deactivate();
    }

    // As sonora-engine --place "Audio Files/Audio 1_01.wav" --arm 2
    // --no-input-monitoring --record: the file in the server's starting
    // document, the take through the server.
    sonora::model::Document doc(sonora::engine_document(s));
    std::vector<sonora::model::ReplicationUpdate> placed;
    CHECK(sonora::place_file(doc, cfg.dir, "Audio Files/Audio 1_01.wav", 0, placed, error));
    s.tracks()[0]->set_rec_armed(false);
    sonora::EngineControlServer::Options opts;
    opts.initial_document = std::make_shared<sonora::model::Snapshot>(doc.current());
    opts.input_monitoring = false;
    s.disk().start();
    s.reader().start();
    {
        auto driver = std::make_unique<LoopDriver>(s, seed);
        sonora::InProcessEngine engine(s, nullptr, opts);
        CHECK(engine.connect(error));
        // The first take left the playhead at its end; sonora-engine starts
        // at 0. The take waits behind the locate.
        CHECK(run(engine, "transport.return_to_zero"));
        CHECK(run(engine, "track.toggle_arm", 1));
        CHECK(run(engine, "transport.record"));
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (s.transport().playhead() < kTakeFrames) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(run(engine, "transport.stop"));
        CHECK(s.take_origin() == 0);
        engine.shutdown();
        driver.reset();
        engine.server().after_deactivate();
    }
    s.reader().stop();

    std::vector<float> played, recorded;
    CHECK(read_mono(dir / "Audio Files" / "Audio 1_01.wav", played));
    CHECK(read_mono(dir / "Audio Files" / "Audio 2_02.wav", recorded));
    CHECK(played.size() == uint64_t{kSourceBlocks} * kQuantum);
    CHECK(recorded.size() >= kTakeFrames);

    const float gain = std::pow(10.0f, -3.0f / 20.0f);
    const std::size_t lag = kLoopPeriods * kQuantum;
    double worst = 0.0;
    std::size_t worst_at = 0;
    for (std::size_t j = 0; j < recorded.size(); ++j) {
        const float want = j >= lag && j - lag < played.size() ? gain * played[j - lag] : 0.0f;
        const double d = std::fabs(static_cast<double>(recorded[j] - want));
        if (d > worst) {
            worst = d;
            worst_at = j;
        }
    }
    std::printf("loopback round %u: %zu frames recorded, worst residual %.3g at frame %zu,"
                " misses %llu, overruns %u\n",
                seed, recorded.size(), worst, worst_at,
                static_cast<unsigned long long>(s.playback_misses()),
                s.disk().total_overruns());
    if (worst >= 1e-6) {
        // Where it went wrong: each period's first frame off, and the lag
        // that would null it.
        const auto onset = [](const std::vector<float>& v) {
            for (std::size_t j = 0; j < v.size(); ++j) {
                if (std::fabs(v[j]) > 1e-4f) return j;
            }
            return v.size();
        };
        std::printf("  onset recorded %zu, played %zu\n", onset(recorded), onset(played));
        int shown = 0;
        for (std::size_t b = 0; b * kQuantum < recorded.size() && shown < 6; ++b) {
            int best_lag = -1;
            for (int l = 0; l <= 8 * static_cast<int>(kQuantum); ++l) {
                bool ok = true;
                for (std::size_t j = b * kQuantum; j < (b + 1) * kQuantum && j < recorded.size(); ++j) {
                    const float w = j >= static_cast<std::size_t>(l) && j - l < played.size()
                                        ? gain * played[j - l] : 0.0f;
                    if (std::fabs(recorded[j] - w) > 1e-6f) { ok = false; break; }
                }
                if (ok) { best_lag = l; break; }
            }
            if (best_lag != static_cast<int>(lag)) {
                std::printf("  period %zu (frames %zu..): nulls at lag %d\n", b,
                            b * kQuantum, best_lag);
                ++shown;
            }
        }
    }
    CHECK(worst < 1e-6);
    CHECK(s.playback_misses() == 0);
    CHECK(s.disk().total_overruns() == 0);
    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    const char* env = std::getenv("SONORA_LOOPBACK_ROUNDS");
    const int rounds = env ? std::max(1, std::atoi(env)) : 3;
    const uint32_t first = static_cast<uint32_t>(std::random_device{}());
    for (int r = 0; r < rounds; ++r) {
        if (loopback_round(first + static_cast<uint32_t>(r))) return 1;
    }
    std::printf("playback_loopback: ok\n");
    return 0;
}
