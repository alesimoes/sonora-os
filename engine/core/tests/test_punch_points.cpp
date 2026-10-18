// SPDX-License-Identifier: GPL-3.0-or-later
//
// Punch points inside a block (timeline-model.md, "Punch in"; BACKLOG DAW 1,
// engine.transport.punch): a take's capture opens and closes at frames the
// control thread set ahead, at the exact sample inside a block, and Auto
// monitoring switches between playback and input at the punch points as a
// crossfade centred on each. Any other change of a track's monitor source
// ramps too, rather than stepping at a block boundary.
//
// Every input sample is its own timeline frame, so a take's file is right
// exactly when each sample equals its TimeReference plus its index.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sonora/fade_curve.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

// The word hook is private to Session; only a test defines this.
namespace sonora {
struct SessionTestAccess {
    static void set_word_hook(Session& s, Session::WordHook hook, void* context) {
        s.word_hook_ = hook;
        s.word_hook_context_ = context;
    }
};
}  // namespace sonora

namespace {

namespace fs = std::filesystem;
using namespace playback_test;
using sonora::FadeCurve;
using sonora::PunchRange;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
constexpr uint32_t kRamp = 64;  // PlaybackConfig::monitor_ramp_frames

bool near(float a, float b, float tol = 1e-5f) {
    const float scale = std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f;
    return std::fabs(a - b) <= tol * scale;
}

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<std::string> sources;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];

    Rig(const std::string& name, int tracks, uint32_t punch_capacity = 8,
        uint32_t boundary_records = 0) {
        dir = fs::temp_directory_path() / ("sonora_test_punch_points_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 96;
        cfg.playback.max_block_frames = 256;
        cfg.playback.monitor_ramp_frames = kRamp;
        cfg.playback.punch_capacity = punch_capacity;
        cfg.boundary_records = boundary_records;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(tracks, std::vector<float>(kBlock, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (auto& t : session->tracks()) t->set_rec_armed(true);
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kBlock, 0.0f);
            out_ptrs[o] = out[o].data();
        }
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    uint32_t source(uint32_t id, int64_t frames) {
        const std::string path = (dir / ("source_" + std::to_string(id) + ".wav")).string();
        write_source(path, id, 1, frames);
        uint32_t slot = 0;
        std::string error;
        session->reader().add_source(path, slot, error);
        return slot;
    }

    // One block whose every input sample is its own timeline frame; the
    // monitor bus lands in `out`. The audio thread is the playhead's only
    // writer and this is the audio thread.
    void block() {
        const uint64_t at = session->transport().playhead();
        for (auto& v : in) {
            for (uint32_t i = 0; i < kBlock; ++i) v[i] = static_cast<float>(at + i);
        }
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = kBlock;
        sonora::backend::ProcessCallback& cb = *session;
        t_counting = true;
        cb.process(b);
        t_counting = false;
    }

    std::string file(int track, int take) const {
        char name[64];
        std::snprintf(name, sizeof name, "Audio %d_%02d.wav", track + 1, take);
        return (dir / "Audio Files" / name).string();
    }

    // Whether no writer slot of `track` holds a take: the disk thread has
    // closed every file it was given.
    bool slots_free(int track) const {
        const sonora::Track& t = *session->tracks()[static_cast<std::size_t>(track)];
        for (int s = 0; s < sonora::Track::kWriterSlots; ++s) {
            if (t.slot_take(s) != 0) return false;
        }
        return true;
    }
};

struct FileCheck {
    bool ok = false;
    uint64_t time_reference = 0;
    int64_t frames = 0;
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
    c.ok = true;
    return c;
}

// The capture opens and closes at the frame asked for, at offsets 0, 1 and
// nframes - 1 inside a block, and the file closes in band with the
// transport still rolling, before any stop.
int test_capture_inside_a_block() {
    Rig rig("capture", 1);
    Session& s = *rig.session;
    // From the third block's start, at offsets 0, 1, nframes - 1 and 17,
    // to the sixth's at 1, nframes - 1, 0 and 40; and both inside one block.
    const uint32_t from_at[] = {2 * kBlock, 2 * kBlock + 1, 3 * kBlock - 1, 2 * kBlock + 17,
                                2 * kBlock + 10};
    const uint32_t to_at[] = {5 * kBlock + 1, 6 * kBlock - 1, 5 * kBlock, 5 * kBlock + 40,
                              2 * kBlock + 50};
    int take = s.next_free_take();
    for (int k = 0; k < 5; ++k, ++take) {
        const uint64_t base = s.transport().playhead();
        PunchRange r;
        r.capture_from = base + from_at[k];
        r.capture_to = base + to_at[k];
        r.monitor_in = r.capture_from;
        r.monitor_out = r.capture_to;
        std::string error;
        CHECK(s.start_punch(take, r, &error));
        for (int b = 0; b < 7; ++b) rig.block();
        // Past the out point, still recording: the file is closed already.
        CHECK(s.transport().state() == TransportState::Recording);
        s.disk().drain_once();
        CHECK(rig.slots_free(0));
        CHECK(s.take_origin() == r.capture_from);
        CHECK(s.stop(&error));
        const FileCheck f = check_file(rig.file(0, take));
        CHECK(f.ok);
        CHECK(f.time_reference == r.capture_from);
        CHECK(f.frames == static_cast<int64_t>(r.capture_to - r.capture_from));
    }
    CHECK(s.boundary_overruns() == 0);
    CHECK(s.tracks()[0]->overruns() == 0);
    return 0;
}

// An open-ended punch whose in point is behind the playhead records from
// the next block's first frame, as a take started on the fly does.
int test_capture_from_behind_the_playhead() {
    Rig rig("behind", 1);
    Session& s = *rig.session;
    s.play();
    for (int b = 0; b < 3; ++b) rig.block();
    const uint64_t p = s.transport().playhead();
    PunchRange r;
    r.capture_from = 5;
    r.monitor_in = 5;
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, r, &error));
    for (int b = 0; b < 3; ++b) rig.block();
    CHECK(s.stop(&error));
    const FileCheck f = check_file(rig.file(0, take));
    CHECK(f.ok);
    CHECK(f.time_reference == p);
    CHECK(f.frames == 3 * static_cast<int64_t>(kBlock));
    return 0;
}

// A range the take could never record, or whose monitoring would hear what
// it does not record, is refused before any file is made.
int test_bad_ranges_are_refused() {
    Rig rig("refused", 1);
    Session& s = *rig.session;
    s.play();
    for (int b = 0; b < 4; ++b) rig.block();
    const uint64_t p = s.transport().playhead();
    const int take = s.next_free_take();
    std::string error;

    PunchRange passed;
    passed.capture_from = 10;
    passed.capture_to = p;
    passed.monitor_in = 10;
    passed.monitor_out = p;
    CHECK(!s.start_punch(take, passed, &error));
    CHECK(error.find("before the playhead") != std::string::npos);

    PunchRange empty;
    empty.capture_from = p + 100;
    empty.capture_to = p + 100;
    error.clear();
    CHECK(!s.start_punch(take, empty, &error));
    CHECK(!error.empty());

    PunchRange unheard;
    unheard.capture_from = p + 100;
    unheard.capture_to = p + 200;
    unheard.monitor_in = p + 50;
    unheard.monitor_out = p + 150;
    error.clear();
    CHECK(!s.start_punch(take, unheard, &error));
    CHECK(error.find("monitor") != std::string::npos);

    CHECK(s.transport().state() == TransportState::Rolling);
    CHECK(!fs::exists(rig.file(0, take)));
    return 0;
}

float ramp_position(uint64_t f, uint64_t at, uint32_t len) {
    const int64_t start = static_cast<int64_t>(at) - static_cast<int64_t>(len / 2);
    const auto x = static_cast<int64_t>(f);
    if (x < start) return 0.0f;
    if (x >= start + static_cast<int64_t>(len)) return 1.0f;
    return static_cast<float>(x - start) / static_cast<float>(len);
}

// Auto monitoring under a punch: playback up to A, the input from A to B,
// playback again after B, switched by a crossfade of the take's length and
// shape centred on each point, across block boundaries. The capture runs
// over its handles either side. `heard` is the crossfade expected: the
// punch's, or monitor_ramp_frames, still centred, when the punch has none.
int monitor_case(const char* name, uint32_t a_at, uint32_t b_at, uint32_t crossfade,
                 FadeCurve shape, uint32_t heard) {
    Rig rig(name, 1);
    Session& s = *rig.session;
    const uint32_t src = rig.source(3, 40 * kChunk);
    SegmentSpec sp;
    sp.end = 40 * kChunk;
    sp.source_slot = src;
    CHECK(publish_plan(s, make_plan(s.plans().next_epoch(), 1, {sp}, kChunk)));
    sonora::Track& t = *s.tracks()[0];
    t.set_pan(-1.0f);  // hard left: the mono pan law's left gain is 1
    t.set_rec_armed(true);
    s.play();
    for (int b = 0; b < 2; ++b) rig.block();

    const uint64_t p = s.transport().playhead();
    constexpr uint64_t kHandle = 100;
    PunchRange r;
    r.monitor_in = p + a_at;
    r.monitor_out = p + b_at;
    r.capture_from = r.monitor_in - kHandle;
    r.capture_to = r.monitor_out + kHandle;
    r.crossfade = crossfade;
    r.shape = shape;
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, r, &error));
    reset_heap_counts();
    for (int b = 0; b < 12; ++b) {
        const uint64_t at = s.transport().playhead();
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const uint64_t f = at + i;
            const float w = std::min(ramp_position(f, r.monitor_in, heard),
                                     1.0f - ramp_position(f, r.monitor_out, heard));
            const float pb = source_value(3, 0, static_cast<int64_t>(f));
            const float want = sonora::fade_in_gain(shape, w) * static_cast<float>(f) +
                               sonora::fade_in_gain(shape, 1.0f - w) * pb;
            if (!near(rig.out[0][i], want)) {
                std::fprintf(stderr, "  frame %llu: %.6f, want %.6f (w %.4f)\n",
                             static_cast<unsigned long long>(f),
                             static_cast<double>(rig.out[0][i]), static_cast<double>(want),
                             static_cast<double>(w));
                CHECK(false);
            }
        }
    }
    CHECK(heap_untouched());
    CHECK(s.playback_misses() == 0);
    CHECK(s.stop(&error));
    const FileCheck f = check_file(rig.file(0, take));
    CHECK(f.ok);
    CHECK(f.time_reference == r.capture_from);
    CHECK(f.frames == static_cast<int64_t>(r.capture_to - r.capture_from));
    std::printf("  %s: heap counter: %s\n", name, heap_counter_scope());
    return 0;
}

int test_auto_monitor_crossfades_at_the_punch_points() {
    if (monitor_case("monitor", 3 * kBlock + 17, 7 * kBlock + 50, 40, FadeCurve::EqualPower,
                     40)) {
        return 1;
    }
    // B - A shorter than the crossfade: the two ramps meet below unity.
    if (monitor_case("monitor_short", 3 * kBlock + 17, 3 * kBlock + 47, 40, FadeCurve::SCurve,
                     40)) {
        return 1;
    }
    // No crossfade: the monitor ramp, centred on A and B, not started there.
    return monitor_case("monitor_cut", 3 * kBlock + 17, 7 * kBlock + 50, 0, FadeCurve::Linear,
                        kRamp);
}

// Off a punch, a change of monitor source is a ramp of
// monitor_ramp_frames from the frame it takes effect, never a step at a
// block boundary: a take started and punched out on the fly, and a mode
// switched from Input to Off.
int test_other_switches_ramp() {
    Rig rig("ramp", 1);
    Session& s = *rig.session;
    const uint32_t src = rig.source(5, 40 * kChunk);
    SegmentSpec sp;
    sp.end = 40 * kChunk;
    sp.source_slot = src;
    CHECK(publish_plan(s, make_plan(s.plans().next_epoch(), 1, {sp}, kChunk)));
    sonora::Track& t = *s.tracks()[0];
    t.set_pan(-1.0f);
    t.set_rec_armed(true);
    s.play();
    for (int b = 0; b < 2; ++b) rig.block();

    // Playback to input, from the take's first frame.
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_recording(take, &error));
    uint64_t at = s.transport().playhead();
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float w = std::min(1.0f, static_cast<float>(i + 1) / kRamp);
        const float pb = source_value(5, 0, static_cast<int64_t>(at + i));
        CHECK(near(rig.out[0][i], w * static_cast<float>(at + i) + (1.0f - w) * pb));
    }
    rig.block();

    // Input back to playback, from the first frame after the punch out.
    CHECK(s.punch_out());
    at = s.transport().playhead();
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float w = std::max(0.0f, 1.0f - static_cast<float>(i + 1) / kRamp);
        const float pb = source_value(5, 0, static_cast<int64_t>(at + i));
        CHECK(near(rig.out[0][i], w * static_cast<float>(at + i) + (1.0f - w) * pb));
    }

    // Input to Off: the input fades out rather than stopping dead.
    t.set_monitor(sonora::Track::Monitor::Input);
    rig.block();
    rig.block();
    t.set_monitor(sonora::Track::Monitor::Off);
    at = s.transport().playhead();
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float w = std::max(0.0f, 1.0f - static_cast<float>(i + 1) / kRamp);
        CHECK(near(rig.out[0][i], w * static_cast<float>(at + i)));
    }
    CHECK(s.stop(&error));
    return 0;
}


// Holds the callback it is called from, right after the transport is loaded
// and before anything is drained, while armed.
struct Stall {
    std::atomic<bool> armed{false};
    std::atomic<bool> stalled{false};
    std::atomic<bool> release{false};
};
void stall_hook(void* context) noexcept SONORA_RT {
    auto* st = static_cast<Stall*>(context);
    if (!st->armed.exchange(false, std::memory_order_acq_rel)) return;
    st->stalled.store(true, std::memory_order_release);
    while (!st->release.load(std::memory_order_acquire)) {
    }
}

// Code review and RT audit of PR #81, N1. A callback loads take A (punched,
// its file already closed at capture_to) and stalls; two punched takes are
// started meanwhile, so its drain takes both records with A still the take
// it plays. A's range must survive: losing it read A as a whole take, opened
// a second window for A after its close, and dropped what it captured.
int test_burst_of_punches_keeps_the_running_range() {
    Rig rig("burst", 1);
    Session& s = *rig.session;
    Stall stall;
    sonora::SessionTestAccess::set_word_hook(s, stall_hook, &stall);
    s.disk().start();
    std::string error;

    const uint64_t p = s.transport().playhead();
    PunchRange a;
    a.capture_from = p + kBlock;
    a.capture_to = p + 2 * kBlock;
    a.monitor_in = a.capture_from;
    a.monitor_out = a.capture_to;
    const int take_a = s.next_free_take();
    CHECK(s.start_punch(take_a, a, &error));
    for (int b = 0; b < 4; ++b) rig.block();
    for (int i = 0; i < 2000 && !rig.slots_free(0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.slots_free(0));

    std::atomic<bool> quit{false};
    std::thread audio([&] {
        while (!quit.load(std::memory_order_acquire)) {
            rig.block();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    stall.armed.store(true, std::memory_order_release);
    for (int i = 0; i < 5000 && !stall.stalled.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stalled = stall.stalled.load(std::memory_order_acquire);
    PunchRange later;
    later.capture_from = p + 1000 * kBlock;
    later.capture_to = later.capture_from + kBlock;
    later.monitor_in = later.capture_from;
    later.monitor_out = later.capture_to;
    const bool b_ok = s.start_punch(take_a + 1, later, &error);
    later.capture_from += 2 * kBlock;
    later.capture_to += 2 * kBlock;
    later.monitor_in = later.capture_from;
    later.monitor_out = later.capture_to;
    const bool c_ok = b_ok && s.start_punch(take_a + 2, later, &error);
    stall.release.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    quit.store(true, std::memory_order_release);
    audio.join();
    CHECK(stalled);
    CHECK(b_ok);
    CHECK(c_ok);
    CHECK(s.stop(&error));

    CHECK(s.dropped_samples() == 0);
    CHECK(s.punches_lost() == 0);
    const FileCheck f = check_file(rig.file(0, take_a));
    CHECK(f.ok);
    CHECK(f.time_reference == a.capture_from);
    CHECK(f.frames == static_cast<int64_t>(kBlock));
    return 0;
}

// N6: a punch ring the audio thread has not drained refuses the next punch,
// with the reason and with no file left behind; once drained, it is taken.
int test_full_punch_ring_is_refused() {
    Rig rig("full_ring", 1, 1);
    Session& s = *rig.session;
    std::string error;
    PunchRange r;
    r.capture_from = 10 * kBlock;
    r.capture_to = 20 * kBlock;
    r.monitor_in = r.capture_from;
    r.monitor_out = r.capture_to;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, r, &error));
    CHECK(s.stop(&error));  // no callback ran: the record is still queued
    CHECK(!s.start_punch(take + 1, r, &error));
    CHECK(error.find("punch") != std::string::npos);
    CHECK(!fs::exists(rig.file(0, take + 1)));
    CHECK(s.transport().state() == TransportState::Stopped);
    rig.block();
    error.clear();
    CHECK(s.start_punch(take + 1, r, &error));
    CHECK(s.stop(&error));
    return 0;
}

// N6: a punch started over a running punch ends that take at the next
// block's first frame and records itself only inside its own range.
int test_punch_over_a_punch() {
    Rig rig("over", 1);
    Session& s = *rig.session;
    s.play();
    rig.block();
    const uint64_t p = s.transport().playhead();
    std::string error;
    PunchRange first;
    first.capture_from = p + 32;
    first.capture_to = p + 100 * kBlock;
    first.monitor_in = first.capture_from;
    first.monitor_out = first.capture_to;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, first, &error));
    for (int b = 0; b < 3; ++b) rig.block();
    const uint64_t q = s.transport().playhead();
    PunchRange second;
    second.capture_from = q + 20;
    second.capture_to = q + 150;
    second.monitor_in = second.capture_from;
    second.monitor_out = second.capture_to;
    CHECK(s.start_punch(take + 1, second, &error));
    for (int b = 0; b < 5; ++b) rig.block();
    CHECK(s.stop(&error));
    const FileCheck a = check_file(rig.file(0, take));
    const FileCheck b = check_file(rig.file(0, take + 1));
    CHECK(a.ok);
    CHECK(b.ok);
    CHECK(a.time_reference == first.capture_from);
    CHECK(a.frames == static_cast<int64_t>(q - first.capture_from));
    CHECK(b.time_reference == second.capture_from);
    CHECK(b.frames == 130);
    return 0;
}

// N6: the out point's close record lost to a full boundary ring. Nothing
// is written past capture_to, so the file still ends there when stop()
// closes it, and the loss is counted.
int test_lost_out_point_close() {
    Rig rig("lost_close", 1, 8, 2);  // the boundary ring holds one record
    Session& s = *rig.session;
    s.play();
    rig.block();
    const uint64_t p = s.transport().playhead();
    std::string error;
    PunchRange r;
    r.capture_from = p + 10;
    r.capture_to = p + 40;
    r.monitor_in = r.capture_from;
    r.monitor_out = r.capture_to;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, r, &error));
    for (int b = 0; b < 3; ++b) rig.block();
    CHECK(s.boundary_overruns() == 1);
    CHECK(s.stop(&error));
    const FileCheck f = check_file(rig.file(0, take));
    CHECK(f.ok);
    CHECK(f.time_reference == r.capture_from);
    CHECK(f.frames == 30);
    CHECK(s.dropped_samples() == 0);
    return 0;
}

// N3: a whole take started with a number a punched take used before (its
// files gone) records the whole take, not the old punch's range.
int test_whole_take_ignores_an_old_punch() {
    Rig rig("stale", 1);
    Session& s = *rig.session;
    s.play();
    rig.block();
    const uint64_t p = s.transport().playhead();
    std::string error;
    PunchRange far;
    far.capture_from = p + 1000 * kBlock;
    far.capture_to = far.capture_from + kBlock;
    far.monitor_in = far.capture_from;
    far.monitor_out = far.capture_to;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, far, &error));
    rig.block();
    CHECK(s.stop(&error));
    s.closed_takes();
    std::error_code ec;
    fs::remove(rig.file(0, take), ec);
    s.play();
    const uint64_t q = s.transport().playhead();
    CHECK(s.start_recording(take, &error));
    for (int b = 0; b < 3; ++b) rig.block();
    CHECK(s.stop(&error));
    const FileCheck f = check_file(rig.file(0, take));
    CHECK(f.ok);
    CHECK(f.time_reference == q);
    CHECK(f.frames == 3 * static_cast<int64_t>(kBlock));
    return 0;
}

// N5: a plan with two source nodes for one engine track would move that
// track's monitor position twice a frame; the audio thread refuses it.
int test_two_sources_for_one_track_are_refused() {
    Rig rig("two_sources", 2);
    Session& s = *rig.session;
    sonora::PlaybackPlan* plan = make_plan(s.plans().next_epoch(), 2, {}, kChunk);
    plan->tracks[1].engine_track = 0;
    CHECK(publish_plan(s, plan));
    rig.block();
    CHECK(s.invalid_plan_blocks() == 1);
    return 0;
}

// Punched and whole takes started, punched out and stopped at random
// against a live audio thread and disk thread, so TSan sees the punch ring
// and the table under contention. Each file holds only its own frames,
// inside its range when it had one.
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
    std::mt19937 rng(20261018);
    struct Started {
        int take;
        bool punched;
        PunchRange range;
    };
    std::vector<Started> started;
    int take = s.next_free_take() - 1;
    std::string error;
    s.play();
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 8));
        const unsigned action = rng() % 5;
        if (s.recording_take() == 0 || action <= 1) {
            const bool punched = action != 1;
            PunchRange r;
            if (punched) {
                // Far enough ahead that the playhead cannot pass it while
                // the files are made, and near enough to be reached.
                const uint64_t p = s.transport().playhead();
                r.capture_from = p + 16 * kBlock + rng() % (8 * kBlock);
                r.capture_to = r.capture_from + 1 + rng() % (16 * kBlock);
                r.monitor_in = r.capture_from;
                r.monitor_out = r.capture_to;
                r.crossfade = rng() % 48;
            }
            const bool ok = punched ? s.start_punch(++take, r, &error)
                                    : s.start_recording(++take, &error);
            if (!ok) {
                std::fprintf(stderr, "  take %d: %s\n", take, error.c_str());
                quit.store(true, std::memory_order_release);
                audio.join();
                return 1;
            }
            started.push_back({take, punched, r});
        } else if (action == 2) {
            s.punch_out();
        } else {
            CHECK(s.stop(&error));
            s.disk().start();
            s.play();
        }
    }
    CHECK(s.stop(&error));
    quit.store(true, std::memory_order_release);
    audio.join();

    for (const Started& st : started) {
        for (int track = 0; track < 2; ++track) {
            const FileCheck c = check_file(rig.file(track, st.take));
            CHECK(c.ok);
            if (!st.punched || c.frames == 0) continue;
            CHECK(c.time_reference >= st.range.capture_from);
            CHECK(c.time_reference + static_cast<uint64_t>(c.frames) <= st.range.capture_to);
        }
    }
    CHECK(s.dropped_samples() == 0);
    CHECK(s.punches_lost() == 0);
    for (const auto& t : s.tracks()) CHECK(t->overruns() == 0);
    std::printf("  %zu takes under contention\n", started.size());
    return 0;
}

}  // namespace

// With a name, that test alone.
int main(int argc, char** argv) {
    struct Test {
        const char* name;
        int (*run)();
    };
    const Test tests[] = {
        {"capture_inside_a_block", test_capture_inside_a_block},
        {"capture_from_behind_the_playhead", test_capture_from_behind_the_playhead},
        {"bad_ranges_are_refused", test_bad_ranges_are_refused},
        {"auto_monitor_crossfades", test_auto_monitor_crossfades_at_the_punch_points},
        {"other_switches_ramp", test_other_switches_ramp},
        {"burst_of_punches", test_burst_of_punches_keeps_the_running_range},
        {"full_punch_ring", test_full_punch_ring_is_refused},
        {"punch_over_a_punch", test_punch_over_a_punch},
        {"lost_out_point_close", test_lost_out_point_close},
        {"whole_take_ignores_an_old_punch", test_whole_take_ignores_an_old_punch},
        {"two_sources_for_one_track", test_two_sources_for_one_track_are_refused},
        {"concurrent_punches", test_concurrent_punches},
    };
    for (const Test& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        if (t.run()) return 1;
    }
    std::printf("punch_points: ok\n");
    return 0;
}
