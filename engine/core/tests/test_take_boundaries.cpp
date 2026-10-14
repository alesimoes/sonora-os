// SPDX-License-Identifier: GPL-3.0-or-later
//
// Takes closed while the transport rolls, with in-band boundary records
// (timeline-model.md, "Closing a take while rolling"; BACKLOG DAW 1).
//
// Every input sample is its own timeline frame, written as 32-bit float, so
// a file is right exactly when each sample equals its TimeReference plus its
// index: a frame from another take, a lost block or a repeated one is a
// wrong value, never a near miss.
//
// The first test is the regression test for the take-epoch race in
// BACKLOG "Known gaps": a callback that loaded Recording for one take and
// stalled across stop() and the next start_recording() wrote the old take's
// block at the head of the new take's file. The stall is real: an insert
// runner's begin() holds the callback after it has loaded the transport.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
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
using sonora::PlaybackPlan;
using sonora::Session;

constexpr uint32_t kBlock = 64;

// Holds one callback in begin() when armed: after process() has loaded the
// transport, before it has written the capture ring.
class StallRunner final : public sonora::InsertChainRunner {
public:
    void arm(std::chrono::milliseconds for_how_long) {
        stall_ns_.store(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(for_how_long)
                                .count()),
                        std::memory_order_relaxed);
        armed_.store(true, std::memory_order_release);
    }
    bool stalled() const { return stalled_.load(std::memory_order_acquire); }
    bool stalled_once() const { return stalled_once_.load(std::memory_order_acquire); }

    void begin(uint64_t now_ns) noexcept SONORA_RT override {
        if (!armed_.exchange(false, std::memory_order_acq_rel)) return;
        stalled_.store(true, std::memory_order_release);
        const uint64_t until = now_ns + stall_ns_.load(std::memory_order_relaxed);
        // A spin, not a sleep: what is under test is a callback descheduled
        // for this long, and the RTSan build must not see a blocking call.
        for (;;) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            if (static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) >= until) {
                break;
            }
        }
        stalled_.store(false, std::memory_order_release);
        stalled_once_.store(true, std::memory_order_release);
    }
    void run(const sonora::PlanNode&, const float* const in[2], float* const out[2],
             uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
    }
    void end() noexcept SONORA_RT override {}

private:
    std::atomic<bool> armed_{false};
    std::atomic<bool> stalled_{false};
    std::atomic<bool> stalled_once_{false};
    std::atomic<uint64_t> stall_ns_{0};
};

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;

    Rig(const std::string& name, int tracks) {
        dir = fs::temp_directory_path() / ("sonora_test_take_boundaries_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = 256;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(tracks, std::vector<float>(kBlock, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (auto& t : session->tracks()) t->set_rec_armed(true);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Track 0 behind an insert chain, so a runner runs in every callback.
    bool publish_chain_on_track_0() {
        const auto tracks = static_cast<uint32_t>(session->tracks().size());
        std::vector<sonora::ChainBinding> chains(tracks);
        chains[0].active = true;
        chains[0].chain_id = 100;
        chains[0].channel_slot = 0;
        chains[0].param_store = 7;
        sonora::PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = sonora::graph_nodes(tracks, 1);
        sizes.edges = sonora::graph_edges(tracks, 1);
        PlaybackPlan* p = sonora::create_plan(sizes);
        p->epoch = session->plans().next_epoch();
        p->chunk_frames = 256;
        p->pan_exponent = 0.5f;
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        sonora::layout_track_graph(*p, chains.data());
        sonora::DiskReader& r = session->reader();
        const uint64_t gen = r.serve(session->plans().current(), p);
        r.run_pass();
        if (r.resident() != gen) return false;
        if (!session->plans().publish(p)) return false;
        r.serve(p, nullptr);
        r.run_pass();
        return true;
    }

    // One block whose every sample is its own timeline frame. The audio
    // thread is the playhead's only writer and this is the audio thread.
    void block() {
        const uint64_t at = session->transport().playhead();
        for (auto& v : in) {
            for (uint32_t i = 0; i < kBlock; ++i) v[i] = static_cast<float>(at + i);
        }
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.nframes = kBlock;
        sonora::backend::ProcessCallback& cb = *session;
        cb.process(b);
    }

    std::string file(int track, int take) const {
        char name[64];
        std::snprintf(name, sizeof name, "Audio %d_%02d.wav", track + 1, take);
        return (dir / "Audio Files" / name).string();
    }
};

// What a take's file holds: each sample is TimeReference + its index.
struct FileCheck {
    bool ok = false;
    uint64_t time_reference = 0;
    int64_t frames = 0;
    float first = 0.0f;
    float last = 0.0f;
};

FileCheck check_file(const std::string& path) {
    FileCheck c;
    sonora::WavReader r;
    std::string error;
    if (!r.open(path, error)) {
        std::fprintf(stderr, "  %s: %s\n", path.c_str(), error.c_str());
        return c;
    }
    c.time_reference = r.time_reference();
    c.frames = r.frames();
    std::vector<float> s(static_cast<std::size_t>(c.frames));
    if (c.frames > 0 && r.read(0, c.frames, s.data()) != c.frames) return c;
    for (int64_t i = 0; i < c.frames; ++i) {
        if (s[static_cast<std::size_t>(i)] != static_cast<float>(c.time_reference + i)) {
            std::fprintf(stderr, "  %s: sample %lld is %.0f, TimeReference + index is %llu\n",
                         path.c_str(), static_cast<long long>(i),
                         static_cast<double>(s[static_cast<std::size_t>(i)]),
                         static_cast<unsigned long long>(c.time_reference + i));
            return c;
        }
    }
    if (c.frames > 0) {
        c.first = s.front();
        c.last = s.back();
    }
    c.ok = true;
    return c;
}

// BACKLOG "Known gaps", the take-epoch race. A callback loads Recording for
// take K and stalls; the operator stops and starts take K + 1 meanwhile;
// the callback then writes K's block. It must land in K, not at the head of
// K + 1, and K + 1 must start on its own first frame.
int test_stalled_callback_across_stop_and_start() {
    Rig rig("stall", 1);
    Session& s = *rig.session;
    StallRunner runner;
    CHECK(rig.publish_chain_on_track_0());
    s.set_insert_chains(&runner);
    s.disk().start();

    std::atomic<bool> quit{false};
    std::thread audio([&] {
        while (!quit.load(std::memory_order_acquire)) {
            rig.block();
            // About real time at 48 kHz, so the ring never fills.
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
    });

    const int k = s.next_free_take();
    std::string error;
    CHECK(s.start_recording(k, &error));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    runner.arm(std::chrono::milliseconds(300));
    for (int i = 0; i < 2000 && !runner.stalled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(runner.stalled());
    // The callback holds Recording for take k now.
    const bool k_ok = s.stop(&error);
    s.disk().start();
    const bool k1_started = s.start_recording(k + 1, &error);
    for (int i = 0; i < 5000 && !runner.stalled_once(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool k1_ok = s.stop(&error);
    quit.store(true, std::memory_order_release);
    audio.join();
    s.set_insert_chains(nullptr);

    CHECK(k_ok);
    CHECK(k1_started);
    CHECK(k1_ok);
    const FileCheck a = check_file(rig.file(0, k));
    const FileCheck b = check_file(rig.file(0, k + 1));
    CHECK(a.ok);
    CHECK(b.ok);
    CHECK(a.frames > 0);
    CHECK(b.frames > 0);
    // Nothing of k reappears in k + 1.
    CHECK(a.last < b.first);
    CHECK(s.tracks()[0]->overruns() == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_stalled_callback_across_stop_and_start() != 0) return 1;
    std::printf("test_take_boundaries: ok\n");
    return 0;
}
