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

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/fade_curve.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

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

    Rig(const std::string& name, int tracks) {
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
    const uint32_t offsets_in[] = {0, 1, kBlock - 1, 17};
    const uint32_t offsets_out[] = {1, kBlock - 1, 0, 40};
    int take = s.next_free_take();
    for (int k = 0; k < 4; ++k, ++take) {
        const uint64_t base = s.transport().playhead();
        PunchRange r;
        r.capture_from = base + 2 * kBlock + offsets_in[k];
        r.capture_to = base + 5 * kBlock + offsets_out[k];
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
// over its handles either side.
int test_auto_monitor_crossfades_at_the_punch_points() {
    Rig rig("monitor", 1);
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
    constexpr uint32_t kX = 40;
    constexpr uint64_t kHandle = 100;
    PunchRange r;
    r.monitor_in = p + 3 * kBlock + 17;
    r.monitor_out = p + 7 * kBlock + 50;
    r.capture_from = r.monitor_in - kHandle;
    r.capture_to = r.monitor_out + kHandle;
    r.crossfade = kX;
    r.shape = FadeCurve::EqualPower;
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_punch(take, r, &error));
    reset_heap_counts();
    for (int b = 0; b < 12; ++b) {
        const uint64_t at = s.transport().playhead();
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const uint64_t f = at + i;
            const float w = std::min(ramp_position(f, r.monitor_in, kX),
                                     1.0f - ramp_position(f, r.monitor_out, kX));
            const float pb = source_value(3, 0, static_cast<int64_t>(f));
            const float want = sonora::fade_in_gain(FadeCurve::EqualPower, w) * static_cast<float>(f) +
                               sonora::fade_in_gain(FadeCurve::EqualPower, 1.0f - w) * pb;
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
    std::printf("  heap counter: %s\n", heap_counter_scope());
    return 0;
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

}  // namespace

int main() {
    if (test_capture_inside_a_block()) return 1;
    if (test_capture_from_behind_the_playhead()) return 1;
    if (test_bad_ranges_are_refused()) return 1;
    if (test_auto_monitor_crossfades_at_the_punch_points()) return 1;
    if (test_other_switches_ramp()) return 1;
    std::printf("punch_points: ok\n");
    return 0;
}
