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
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

// Every operator new on the thread inside process() is counted: a boundary
// record must cost the audio thread no allocation (CLAUDE.md). The RTSan
// job checks the same calls for malloc and locks.
namespace {
thread_local bool t_counting = false;
std::atomic<long> g_allocations{0};
void* counted(std::size_t n) {
    if (t_counting) g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n > 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t n) { return counted(n); }
void* operator new[](std::size_t n) { return counted(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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

    Rig(const std::string& name, int tracks, uint32_t boundary_records = 0) {
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
        cfg.boundary_records = boundary_records;
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
        t_counting = true;
        cb.process(b);
        t_counting = false;
    }
    void blocks(int n) {
        for (int i = 0; i < n; ++i) block();
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

// The closed takes' files, by take and track.
using Closed = std::map<int, std::map<int, sonora::DiskThread::ClosedFile>>;
void add(Closed& into, std::vector<Session::ClosedTake> takes) {
    for (auto& t : takes) {
        for (auto& f : t.files) into[t.take][f.track] = std::move(f);
    }
}

// Punch in on the fly, a take started over another, punch out on the fly,
// punch in again, stop: the transport never stops until the end, and every
// file holds exactly its own frames, the boundaries on the blocks where the
// audio thread met them. The disk thread is not started, so closed_takes()
// and stop() drain on this thread and the test is deterministic.
int test_takes_while_rolling() {
    Rig rig("rolling", 2);
    Session& s = *rig.session;
    std::string error;
    Closed closed;
    s.play();
    rig.blocks(3);
    CHECK(s.start_recording(1, &error));
    rig.blocks(4);
    CHECK(s.start_recording(2, &error));
    CHECK(s.recording_take() == 2);
    rig.blocks(3);
    // Take 1 is reported closed while take 2 records: no transport stop.
    add(closed, s.closed_takes());
    CHECK(closed.size() == 1 && closed.count(1) == 1);
    CHECK(s.punch_out());
    CHECK(!s.punch_out());
    CHECK(s.transport().state() == sonora::TransportState::Rolling);
    rig.blocks(2);
    add(closed, s.closed_takes());
    CHECK(closed.count(2) == 1);
    CHECK(s.start_recording(3, &error));
    rig.blocks(2);
    CHECK(s.stop(&error));
    add(closed, s.closed_takes());
    CHECK(closed.size() == 3);
    CHECK(s.transport().playhead() == 14 * kBlock);
    CHECK(g_allocations.load() == 0);

    struct Want {
        int take;
        uint64_t origin;
        int64_t frames;
    };
    for (const Want w : {Want{1, 3 * kBlock, 4 * kBlock}, Want{2, 7 * kBlock, 3 * kBlock},
                         Want{3, 12 * kBlock, 2 * kBlock}}) {
        for (int track = 0; track < 2; ++track) {
            const FileCheck c = check_file(rig.file(track, w.take));
            CHECK(c.ok);
            CHECK(c.time_reference == w.origin);
            CHECK(c.frames == w.frames);
            // "Take closed, length N": the length reported is the length on
            // disk.
            const sonora::DiskThread::ClosedFile& f = closed[w.take][track];
            CHECK(f.path == rig.file(track, w.take));
            CHECK(f.frames == static_cast<uint64_t>(w.frames));
            CHECK(f.failure.empty());
        }
    }
    for (const auto& t : s.tracks()) CHECK(t->overruns() == 0);
    CHECK(s.boundary_overruns() == 0);
    return 0;
}

// No callback after the stop: a stopped backend, or a test with none. The
// files close at once, at what reached the ring, and the next take starts.
int test_stop_without_a_callback() {
    Rig rig("no_callback", 1);
    Session& s = *rig.session;
    std::string error;
    CHECK(s.start_recording(1, &error));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(s.stop(&error));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    CHECK(!s.tracks()[0]->writer().is_open());
    Closed closed;
    add(closed, s.closed_takes());
    CHECK(closed.count(1) == 1 && closed[1][0].frames == 0);

    CHECK(s.start_recording(2, &error));
    rig.blocks(2);
    CHECK(s.stop(&error));
    const FileCheck c = check_file(rig.file(0, 2));
    CHECK(c.ok && c.frames == 2 * kBlock && c.time_reference == 0);
    return 0;
}

// A full boundary ring is an overrun, never a wait: a track whose open
// record did not fit starts its take a block later, counted, and its file
// is still exactly its own frames from its own TimeReference.
int test_full_boundary_ring() {
    Rig rig("full", 3, 2);  // holds one record
    Session& s = *rig.session;
    std::string error;
    CHECK(s.start_recording(1, &error));
    rig.block();
    CHECK(s.boundary_overruns() == 2);
    CHECK(s.tracks()[0]->overruns() == 0);
    CHECK(s.tracks()[1]->overruns() == 1);
    CHECK(s.tracks()[2]->overruns() == 1);
    for (int i = 0; i < 4; ++i) {
        s.closed_takes();  // drains, as the disk thread would
        rig.block();
    }
    CHECK(s.stop(&error));
    CHECK(g_allocations.load() == 0);
    for (int track = 0; track < 3; ++track) {
        const FileCheck c = check_file(rig.file(track, 1));
        CHECK(c.ok);
        CHECK(c.time_reference == static_cast<uint64_t>(track) * kBlock);
        CHECK(c.frames == (5 - track) * static_cast<int64_t>(kBlock));
    }
    return 0;
}

// Two takes started over a third before any callback ran hold both slots:
// the next is refused with the reason, not waited for past a bound, and
// leaves no file behind.
int test_no_free_slot_is_refused() {
    Rig rig("no_slot", 1);
    Session& s = *rig.session;
    std::string error;
    CHECK(s.start_recording(1, &error));
    rig.block();
    CHECK(s.start_recording(2, &error));
    CHECK(!s.start_recording(3, &error));
    CHECK(error.find("still being closed") != std::string::npos);
    CHECK(!fs::exists(rig.file(0, 3)));
    CHECK(s.recording_take() == 2);
    rig.block();
    CHECK(s.stop(&error));
    const FileCheck a = check_file(rig.file(0, 1));
    const FileCheck b = check_file(rig.file(0, 2));
    CHECK(a.ok && a.frames == kBlock);
    CHECK(b.ok && b.frames == kBlock && b.time_reference == kBlock);
    return 0;
}

// The audio thread on its own thread, the disk thread running, and the
// control thread punching in and out, starting takes over takes and
// stopping, at random moments. Every file holds exactly its own frames, and
// a take started over another follows it with no frame lost or repeated.
// CI runs this under TSan.
int test_concurrent_punches() {
    Rig rig("stress", 2);
    Session& s = *rig.session;
    s.disk().start();
    std::atomic<bool> quit{false};
    std::thread audio([&] {
        while (!quit.load(std::memory_order_acquire)) {
            rig.block();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    std::mt19937 rng(20261014);
    std::vector<std::pair<int, int>> over;  // (take, the take it was started over)
    int take = 0;
    int started = 0;
    std::string error;
    s.play();
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 12));
        const bool recording = s.recording_take() != 0;
        const unsigned action = rng() % 4;
        if (!recording || action == 0) {
            const int before = s.recording_take();
            if (!s.start_recording(++take, &error)) {
                std::fprintf(stderr, "  take %d: %s\n", take, error.c_str());
                return 1;
            }
            ++started;
            if (before != 0) over.emplace_back(take, before);
        } else if (action == 1) {
            CHECK(s.punch_out());
        } else {
            CHECK(s.stop(&error));
            s.disk().start();
            s.play();
        }
    }
    CHECK(s.stop(&error));
    quit.store(true, std::memory_order_release);
    audio.join();

    Closed closed;
    add(closed, s.closed_takes());
    CHECK(static_cast<int>(closed.size()) == started);
    for (int k = 1; k <= take; ++k) {
        for (int track = 0; track < 2; ++track) {
            const FileCheck c = check_file(rig.file(track, k));
            CHECK(c.ok);
            CHECK(closed[k][track].frames == static_cast<uint64_t>(c.frames));
        }
    }
    for (const auto& p : over) {
        const FileCheck a = check_file(rig.file(0, p.second));
        const FileCheck b = check_file(rig.file(0, p.first));
        if (a.frames == 0 || b.frames == 0) continue;
        CHECK(static_cast<uint64_t>(a.last) + 1 == static_cast<uint64_t>(b.first));
    }
    for (const auto& t : s.tracks()) CHECK(t->overruns() == 0);
    std::printf("  %d takes, %zu started over another\n", started, over.size());
    return 0;
}

}  // namespace

int main() {
    if (test_stalled_callback_across_stop_and_start() != 0) return 1;
    if (test_takes_while_rolling() != 0) return 1;
    if (test_stop_without_a_callback() != 0) return 1;
    if (test_full_boundary_ring() != 0) return 1;
    if (test_no_free_slot_is_refused() != 0) return 1;
    if (test_concurrent_punches() != 0) return 1;
    std::printf("test_take_boundaries: ok\n");
    return 0;
}
