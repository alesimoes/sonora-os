// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read automation (ADR-0027 §2, §4): a lane's value at a frame, the lanes
// played from the plan on a 32-frame timeline grid, the same output however
// the callbacks are cut, a locate mid-ramp crossfaded over one callback and
// exact after it, a loop wrap taking the lane at the loop start with no
// fade, Off playing the static fader, a pan lane, and no allocation.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/session.h"

namespace {

namespace fs = std::filesystem;
using namespace playback_test;
using sonora::kAutoGridFrames;
using sonora::kAutoPan;
using sonora::kAutoVolume;
using sonora::PlanAutoPoint;
using sonora::Session;
using sonora::Track;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kMaxBlock = 256;
constexpr uint32_t kBlock = 97;
constexpr int64_t kSourceFrames = 40 * kChunk;

// 0 dB at 0 down to -40 dB at 4000, then a step back to -6 dB at 6000.
const std::vector<PlanAutoPoint> kRide = {{0, 0.0f}, {4000, -40.0f}, {6000, -40.0f},
                                          {6000, -6.0f}};

float gain_of_db(float db) { return std::pow(10.0f, db / 20.0f); }

// What the engine should apply at frame t for a volume lane, by the grid
// rule: exact at multiples of 32, linear between.
float expected_gain(const std::vector<PlanAutoPoint>& lane, int64_t t) {
    const auto n = static_cast<uint32_t>(lane.size());
    const int64_t g0 = t - t % kAutoGridFrames;
    const float c0 = gain_of_db(sonora::auto_lane_value(lane.data(), n, g0));
    const float c1 = gain_of_db(sonora::auto_lane_value(lane.data(), n, g0 + kAutoGridFrames));
    return c0 + (c1 - c0) * (static_cast<float>(t - g0) / static_cast<float>(kAutoGridFrames));
}

bool near(float a, float b, float tol = 2e-5f) {
    const float scale = std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f;
    return std::fabs(a - b) <= tol * scale;
}

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    const float* in_ptrs[1];
    std::vector<float> out[2];
    float* out_ptrs[2];
    uint32_t slot = 0;

    explicit Rig(const std::string& name) {
        dir = fs::temp_directory_path() / ("sonora_test_automation_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = 1;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 96;
        cfg.playback.max_block_frames = kMaxBlock;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(kMaxBlock, 0.0f);
        in_ptrs[0] = in.data();
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kMaxBlock, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        session->tracks()[0]->set_rec_armed(false);
        // Mono, hard left: out[0] is the signal times the volume, exactly.
        session->tracks()[0]->set_pan(-1.0f);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // The source and a plan whose track plays it with these lanes.
    bool load(const std::vector<PlanAutoPoint>& volume,
              const std::vector<PlanAutoPoint>& pan = {}) {
        Session& s = *session;
        const std::string path = (dir / "source.wav").string();
        if (!write_source(path, 5, 1, kSourceFrames)) return false;
        std::string error;
        if (!s.reader().add_source(path, slot, error)) return false;
        return publish(volume, pan);
    }

    bool publish(const std::vector<PlanAutoPoint>& volume,
                 const std::vector<PlanAutoPoint>& pan) {
        Session& s = *session;
        SegmentSpec sp;
        sp.end = kSourceFrames;
        sp.source_slot = slot;
        const auto total = static_cast<uint32_t>(volume.size() + pan.size());
        sonora::PlaybackPlan* p = make_plan(s.plans().next_epoch(), 1, {sp}, kChunk, 0, 0, total);
        uint32_t at = 0;
        p->tracks[0].auto_first[kAutoVolume] = at;
        p->tracks[0].auto_count[kAutoVolume] = static_cast<uint32_t>(volume.size());
        for (const PlanAutoPoint& v : volume) p->auto_points[at++] = v;
        p->tracks[0].auto_first[kAutoPan] = at;
        p->tracks[0].auto_count[kAutoPan] = static_cast<uint32_t>(pan.size());
        for (const PlanAutoPoint& v : pan) p->auto_points[at++] = v;
        if (!publish_plan(s, p)) return false;
        s.reader().run_pass();
        return true;
    }

    bool loop(uint64_t a, uint64_t b) {
        Session& s = *session;
        std::string error;
        if (!s.set_loop(a, b, &error)) return false;
        for (int k = 0; k < 8 && s.loop_pending(); ++k) {
            s.reader().run_pass();
            s.pump_loop();
        }
        return !s.loop_pending();
    }

    // Stopped there, with a settled callback after the one that jumped.
    bool park(uint64_t frame) {
        Session& s = *session;
        if (!s.post_locate(frame)) return false;
        s.reader().run_pass();
        block();
        s.reader().run_pass();
        block();
        return s.transport().playhead() == frame;
    }

    void block(uint32_t n = kBlock) {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs;
        b.input_count = 1;
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        sonora::backend::ProcessCallback& cb = *session;
        t_counting = true;
        cb.process(b);
        t_counting = false;
        session->reader().run_pass();
    }
};

int test_lane_value() {
    const std::vector<PlanAutoPoint> lane = {
        {1000, 0.0f}, {2000, -20.0f}, {2000, -6.0f}, {3000, -6.0f}};
    const auto n = static_cast<uint32_t>(lane.size());
    CHECK(sonora::auto_lane_value(lane.data(), n, 0) == 0.0f);        // held before
    CHECK(sonora::auto_lane_value(lane.data(), n, 1000) == 0.0f);
    CHECK(sonora::auto_lane_value(lane.data(), n, 1500) == -10.0f);   // linear
    CHECK(near(sonora::auto_lane_value(lane.data(), n, 1999), -19.98f));
    CHECK(sonora::auto_lane_value(lane.data(), n, 2000) == -6.0f);    // the step's second
    CHECK(sonora::auto_lane_value(lane.data(), n, 2500) == -6.0f);
    CHECK(sonora::auto_lane_value(lane.data(), n, 1 << 30) == -6.0f); // held after
    const PlanAutoPoint one{500, 3.0f};
    CHECK(sonora::auto_lane_value(&one, 1, 0) == 3.0f);
    CHECK(sonora::auto_lane_value(&one, 1, 9000) == 3.0f);
    return 0;
}

// Rolled from `start` for `blocks`, every frame against the lane.
bool rolls_the_lane(Rig& rig, uint64_t start, int blocks, const char* what) {
    uint64_t t = start;
    for (int b = 0; b < blocks; ++b) {
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i, ++t) {
            const auto f = static_cast<int64_t>(t);
            const float want = source_value(5, 0, f) * expected_gain(kRide, f);
            if (!near(rig.out[0][i], want)) {
                std::fprintf(stderr, "  %s: frame %lld plays %.7f, want %.7f\n", what,
                             static_cast<long long>(f), static_cast<double>(rig.out[0][i]),
                             static_cast<double>(want));
                return false;
            }
        }
    }
    return true;
}

int test_read_plays_the_lane() {
    Rig rig("read");
    CHECK(rig.load(kRide));
    CHECK(rig.park(3500));
    rig.session->play();
    reset_heap_counts();
    // Across the bottom of the ramp, the hold and the step at 6000.
    CHECK(rolls_the_lane(rig, 3500, 30, "read"));
    CHECK(heap_untouched());
    return 0;
}

// The output for frames [start, start + total) cut into `sizes`, repeated.
std::vector<float> rolled(const std::vector<uint32_t>& sizes, uint64_t start, uint32_t total) {
    Rig rig("split");
    std::vector<float> got;
    if (!rig.load(kRide) || !rig.park(start)) return got;
    rig.session->play();
    for (std::size_t k = 0; got.size() < total; ++k) {
        const uint32_t n = sizes[k % sizes.size()];
        rig.block(n);
        got.insert(got.end(), rig.out[0].begin(), rig.out[0].begin() + n);
    }
    got.resize(total);
    return got;
}

int test_block_split_invariance() {
    const std::vector<float> a = rolled({64}, 3000, 4000);
    const std::vector<float> b = rolled({37, 101, 256, 1, 200, 13, 128}, 3000, 4000);
    CHECK(a.size() == 4000 && b.size() == 4000);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            std::fprintf(stderr, "  frame %zu: %.9f against %.9f\n", 3000 + i,
                         static_cast<double>(a[i]), static_cast<double>(b[i]));
            return 1;
        }
    }
    return 0;
}

int test_locate_mid_ramp() {
    Rig rig("locate");
    CHECK(rig.load(kRide));
    CHECK(rig.park(1000));
    rig.session->play();
    CHECK(rolls_the_lane(rig, 1000, 6, "before"));
    const int64_t last = 1000 + 6 * kBlock - 1;
    const float was = expected_gain(kRide, last);
    // Rolling, to the steep part of the ramp: the callback that lands there
    // crossfades from where the fader was.
    // The locate gate, as test_playback_mix does it: the target resident
    // first, so the callback plays it rather than a miss.
    sonora::DiskReader& r = rig.session->reader();
    const uint64_t gen = r.serve(rig.session->plans().current(), nullptr, 3000,
                                 rig.session->transport().locates_applied() + 1);
    r.run_pass();
    CHECK(r.resident() == gen);
    const uint64_t misses = rig.session->playback_misses();
    CHECK(rig.session->post_locate(3000));
    rig.block();
    CHECK(rig.session->playback_misses() == misses);
    CHECK(rig.session->transport().playhead() == 3000 + kBlock);
    const float first = rig.out[0][0] / source_value(5, 0, 3000);
    const float lane = expected_gain(kRide, 3000);
    if (!(std::fabs(first - was) < std::fabs(lane - was) * 0.05f)) {
        std::fprintf(stderr, "  first %.6f, was %.6f, lane %.6f\n", static_cast<double>(first),
                     static_cast<double>(was), static_cast<double>(lane));
    }
    CHECK(std::fabs(first - was) < std::fabs(lane - was) * 0.05f);
    const auto end = static_cast<int64_t>(3000 + kBlock - 1);
    CHECK(near(rig.out[0][kBlock - 1], source_value(5, 0, end) * expected_gain(kRide, end)));
    // And the lane itself from the next callback.
    CHECK(rolls_the_lane(rig, 3000 + kBlock, 4, "after"));
    return 0;
}

int test_loop_wrap() {
    Rig rig("loop");
    CHECK(rig.load(kRide));
    CHECK(rig.loop(1000, 3000));
    CHECK(rig.park(900));
    rig.session->play();
    uint64_t t = 900;
    // Three passes; blocks of 97 straddle B at different offsets.
    for (int b = 0; b < 70; ++b) {
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const auto f = static_cast<int64_t>(t);
            const float want = source_value(5, 0, f) * expected_gain(kRide, f);
            if (!near(rig.out[0][i], want)) {
                std::fprintf(stderr, "  block %d frame %u (timeline %lld): %.7f, want %.7f\n", b,
                             i, static_cast<long long>(f), static_cast<double>(rig.out[0][i]),
                             static_cast<double>(want));
                return 1;
            }
            if (++t == 3000) t = 1000;
        }
    }
    return 0;
}

int test_off_and_pan() {
    Rig rig("off");
    // Pan from hard left to hard right over [0, 4000); no volume lane.
    const std::vector<PlanAutoPoint> sweep = {{0, -1.0f}, {4000, 1.0f}};
    CHECK(rig.load({}, sweep));
    Track& track = *rig.session->tracks()[0];
    CHECK(rig.park(2000));
    rig.session->play();
    rig.block();
    // Frame 2016 is on the grid, where the pan is the lane's, about 0.008;
    // a mono track pans by sqrt((1 -+ pan) / 2) at pan_exponent 0.5.
    const int64_t f = 2016;
    const uint32_t i = static_cast<uint32_t>(f - 2000);
    const float pan = sonora::auto_lane_value(sweep.data(), 2, f);
    CHECK(near(pan, 0.008f, 1e-4f));
    CHECK(near(rig.out[0][i], source_value(5, 0, f) * std::sqrt((1.0f - pan) * 0.5f)));
    CHECK(near(rig.out[1][i], source_value(5, 0, f) * std::sqrt((1.0f + pan) * 0.5f)));

    // Off: the static pan, hard left, after one crossfading callback.
    track.set_automation(Track::Automation::Off);
    rig.block();
    rig.block();
    for (uint32_t k = 0; k < kBlock; ++k) {
        const auto g = static_cast<int64_t>(2000 + 2 * kBlock + k);
        CHECK(rig.out[0][k] == source_value(5, 0, g));
        CHECK(rig.out[1][k] == 0.0f);
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)(), const char* name) {
        ++run;
        if (f() != 0) {
            std::fprintf(stderr, "  in %s\n", name);
            ++failed;
        }
    };
    t(test_lane_value, "test_lane_value");
    t(test_read_plays_the_lane, "test_read_plays_the_lane");
    t(test_block_split_invariance, "test_block_split_invariance");
    t(test_locate_mid_ramp, "test_locate_mid_ramp");
    t(test_loop_wrap, "test_loop_wrap");
    t(test_off_and_pan, "test_off_and_pan");
    if (failed != 0) {
        std::fprintf(stderr, "automation: %d of %d failed (heap counter: %s)\n", failed, run,
                     heap_counter_scope());
        return 1;
    }
    std::printf("automation: %d passed (heap counter: %s)\n", run, heap_counter_scope());
    return 0;
}
