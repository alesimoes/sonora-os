// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read automation (ADR-0027 §2, §4): a lane's value at a frame, the lanes
// played from the plan on a 32-frame timeline grid, the same output however
// the callbacks are cut, a locate mid-ramp crossfaded over one callback and
// exact after it, a loop wrap taking the lane at the loop start with no
// fade, Off playing the static fader, a pan lane, a static parameter's change
// ramped on a partly automated track, and no allocation. Live writes (§3):
// Touch plays and stamps the fader while held and returns to the lane over
// the return time, Latch holds until the stop, Write writes every parameter
// and splits a span at the loop's end, a full ring is counted.
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

// The coefficient each output frame of the last block applied, from `start`.
std::vector<float> applied(const Rig& rig, int side, int64_t start) {
    std::vector<float> g(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        g[i] = rig.out[side][i] / source_value(5, 0, start + i);
    }
    return g;
}

// From `from` to `to` in equal steps across the block, ending on `to` exactly:
// the ADR-0009 §4.5 ramp, not a step on its first frame.
bool ramps(const std::vector<float>& g, float from, float to, const char* what) {
    const float step = (to - from) / static_cast<float>(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float want = from + step * static_cast<float>(i + 1);
        if (!near(g[i], want, 1e-4f)) {
            std::fprintf(stderr, "  %s: frame %u applies %.6f, want %.6f\n", what, i,
                         static_cast<double>(g[i]), static_cast<double>(want));
            return false;
        }
    }
    return true;
}

bool holds(const std::vector<float>& g, float value, const char* what) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (!near(g[i], value, 1e-4f)) {
            std::fprintf(stderr, "  %s: frame %u applies %.6f, want %.6f\n", what, i,
                         static_cast<double>(g[i]), static_cast<double>(value));
            return false;
        }
    }
    return true;
}

int test_level_change_on_a_pan_lane_ramps() {
    Rig rig("level_on_pan");
    // Flat hard left: out[0] is the signal times the static level.
    CHECK(rig.load({}, {{0, -1.0f}}));
    Track& track = *rig.session->tracks()[0];
    const float low = gain_of_db(-40.0f);
    track.set_gain(low);
    CHECK(rig.park(2000));
    rig.session->play();
    rig.block();
    int64_t t = 2000 + kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), low, "settled"));
    t += kBlock;

    track.set_gain(1.0f);
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(ramps(applied(rig, 0, t), low, 1.0f, "up"));
    t += kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), 1.0f, "after"));
    return 0;
}

int test_pan_change_on_a_volume_lane_ramps() {
    Rig rig("pan_on_level");
    CHECK(rig.load({{0, 0.0f}}));
    Track& track = *rig.session->tracks()[0];
    CHECK(rig.park(2000));
    rig.session->play();
    rig.block();
    int64_t t = 2000 + kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), 1.0f, "settled left"));
    CHECK(holds(applied(rig, 1, t), 0.0f, "settled right"));
    t += kBlock;

    track.set_pan(1.0f);
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(ramps(applied(rig, 0, t), 1.0f, 0.0f, "left"));
    CHECK(ramps(applied(rig, 1, t), 0.0f, 1.0f, "right"));
    t += kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), 0.0f, "after left"));
    CHECK(holds(applied(rig, 1, t), 1.0f, "after right"));
    return 0;
}

// --- live writes (ADR-0027 §3) ---

using sonora::AutomationWrite;

std::vector<AutomationWrite> drained(Session& s) {
    std::vector<AutomationWrite> got(4096);
    got.resize(s.take_automation_writes(got.data(), got.size()));
    return got;
}

// What a Touch return from `from_db` at `r` toward a flat lane at `lane_db`
// applies at frame f, by the grid rule.
float expected_return(float from_db, float lane_db, int64_t r, int64_t len, int64_t f) {
    const auto at = [&](int64_t g) {
        if (g < r - kAutoGridFrames || g >= r + len) return gain_of_db(lane_db);
        if (g < r) return gain_of_db(from_db);
        const float x = static_cast<float>(g - r) / static_cast<float>(len);
        return gain_of_db(from_db + (lane_db - from_db) * x);
    };
    const int64_t g0 = f - f % kAutoGridFrames;
    const float c0 = at(g0);
    const float c1 = at(g0 + kAutoGridFrames);
    return c0 + (c1 - c0) * (static_cast<float>(f - g0) / static_cast<float>(kAutoGridFrames));
}

int test_touch_plays_and_writes_the_fader() {
    Rig rig("touch");
    CHECK(rig.load({{0, -40.0f}}));
    Session& s = *rig.session;
    Track& track = *s.tracks()[0];
    track.set_automation(Track::Automation::Touch);
    CHECK(rig.park(2000));
    s.play();
    rig.block();
    int64_t t = 2000 + kBlock;
    rig.block();
    const float lane = gain_of_db(-40.0f);
    CHECK(holds(applied(rig, 0, t), lane, "untouched"));
    t += kBlock;
    CHECK(drained(s).empty());

    // Held: the fader, crossfaded in from the lane over one callback.
    track.set_gain(1.0f);
    track.set_touched(kAutoVolume, true);
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(ramps(applied(rig, 0, t), lane, 1.0f, "touch"));
    const int64_t first = t;
    t += kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), 1.0f, "held"));
    t += kBlock;
    std::vector<AutomationWrite> w = drained(s);
    CHECK(w.size() == 2);
    if (w.size() != 2) return 1;
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(w[i].pass == 1 && w[i].track == 0 && w[i].param == kAutoVolume);
        CHECK(w[i].frame == static_cast<uint64_t>(first) + i * kBlock && w[i].frames == kBlock);
        CHECK(w[i].value == 1.0f);
    }

    // Released: back to the lane over the return time, from 0 dB.
    track.set_touched(kAutoVolume, false);
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    const int64_t r = t;
    const auto len = static_cast<int64_t>(s.touch_return_frames());
    CHECK(len == 12000);
    w = drained(s);
    CHECK(w.size() == 1 && w[0].frames == 0 && w[0].frame == static_cast<uint64_t>(r) &&
          w[0].value == 1.0f);
    t += kBlock;
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float want = expected_return(0.0f, -40.0f, r, len, t + i);
        CHECK(near(applied(rig, 0, t)[i], want, 1e-4f));
    }
    // Not snapped to the lane: still near the fader a few blocks on.
    CHECK(applied(rig, 0, t)[kBlock - 1] > 0.9f);
    CHECK(drained(s).empty());

    // A stop ends the pass, and the release is not replayed by a locate.
    CHECK(s.automation_passes_ended() == 0);
    s.stop_playback();
    rig.block();
    CHECK(s.automation_passes_ended() == 1);
    CHECK(rig.park(2000));
    s.play();
    rig.block();
    rig.block();
    CHECK(holds(applied(rig, 0, 2000 + kBlock), lane, "after the pass"));
    return 0;
}

int test_latch_holds_until_stop() {
    Rig rig("latch");
    CHECK(rig.load({{0, -40.0f}}));
    Session& s = *rig.session;
    Track& track = *s.tracks()[0];
    track.set_automation(Track::Automation::Latch);
    track.set_gain(0.5f);
    CHECK(rig.park(2000));
    s.play();
    rig.block();
    track.set_touched(kAutoVolume, true);
    rig.block();
    track.set_touched(kAutoVolume, false);
    rig.block();
    int64_t t = 2000 + 3 * kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), 0.5f, "latched after the release"));
    const std::vector<AutomationWrite> w = drained(s);
    // Touch and every block since, no release mark.
    CHECK(w.size() == 3);
    for (const AutomationWrite& x : w) CHECK(x.frames == kBlock && x.value == 0.5f);

    s.stop_playback();
    rig.block();
    CHECK(s.automation_passes_ended() == 1);
    CHECK(rig.park(2000));
    s.play();
    rig.block();
    rig.block();
    CHECK(holds(applied(rig, 0, 2000 + kBlock), gain_of_db(-40.0f), "lane on the next pass"));
    CHECK(drained(s).empty());
    return 0;
}

// #178 review: a Load while rolling keeps the roll going on the new
// session's plan, whose tracks share engine indices with the old one's. A
// Latch held from the old session must not carry over: the track plays its
// lane again at once and stops writing, while the pass number stays the
// roll's.
int test_forgotten_latches_play_the_lane() {
    Rig rig("forget");
    CHECK(rig.load({{0, -40.0f}}));
    Session& s = *rig.session;
    Track& track = *s.tracks()[0];
    track.set_automation(Track::Automation::Latch);
    track.set_gain(0.5f);
    CHECK(s.automation_pass() == 0);
    CHECK(rig.park(2000));
    s.play();
    rig.block();
    track.set_touched(kAutoVolume, true);
    rig.block();
    track.set_touched(kAutoVolume, false);
    rig.block();
    CHECK(s.automation_pass() == 1);
    CHECK(drained(s).size() == 2);

    s.forget_latches();
    int64_t t = 2000 + 3 * kBlock;
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    const float lane = gain_of_db(-40.0f);
    CHECK(ramps(applied(rig, 0, t), 0.5f, lane, "unlatched"));
    t += kBlock;
    rig.block();
    CHECK(holds(applied(rig, 0, t), lane, "the lane"));
    CHECK(drained(s).empty());
    CHECK(s.automation_pass() == 1);

    s.stop_playback();
    rig.block();
    CHECK(rig.park(2000));
    s.play();
    rig.block();
    CHECK(s.automation_pass() == 2);
    return 0;
}

int test_write_writes_every_parameter_and_splits_at_the_wrap() {
    Rig rig("write");
    CHECK(rig.load({{0, -40.0f}}));
    Session& s = *rig.session;
    Track& track = *s.tracks()[0];
    track.set_automation(Track::Automation::Write);
    track.set_gain(0.25f);
    CHECK(rig.loop(1000, 3000));
    CHECK(rig.park(2950));
    s.play();
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    // The fader, never the lane.
    CHECK(near(rig.out[0][0], source_value(5, 0, 2950) * 0.25f, 1e-4f));
    const std::vector<AutomationWrite> w = drained(s);
    // Volume and pan, each [2950, 3000) then [1000, 1047).
    CHECK(w.size() == 4);
    if (w.size() != 4) return 1;
    CHECK(w[0].param == kAutoVolume && w[0].frame == 2950 && w[0].frames == 50);
    CHECK(w[1].param == kAutoVolume && w[1].frame == 1000 && w[1].frames == kBlock - 50);
    CHECK(w[0].value == 0.25f);
    CHECK(w[2].param == kAutoPan && w[2].frame == 2950 && w[3].frame == 1000);
    CHECK(w[2].value == -1.0f);
    return 0;
}

int test_a_full_ring_is_counted() {
    Rig rig("full");
    CHECK(rig.load({}));
    Session& s = *rig.session;
    s.tracks()[0]->set_automation(Track::Automation::Write);
    CHECK(rig.loop(1000, 6000));
    CHECK(rig.park(1000));
    s.play();
    reset_heap_counts();
    // Never drained: two writes a block, more blocks than the ring holds.
    for (int b = 0; b < 1200; ++b) rig.block();
    CHECK(heap_untouched());
    CHECK(s.automation_write_overruns() > 0);
    const std::vector<AutomationWrite> w = drained(s);
    CHECK(!w.empty() && w.front().frame == 1000);
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
    t(test_level_change_on_a_pan_lane_ramps, "test_level_change_on_a_pan_lane_ramps");
    t(test_pan_change_on_a_volume_lane_ramps, "test_pan_change_on_a_volume_lane_ramps");
    t(test_touch_plays_and_writes_the_fader, "test_touch_plays_and_writes_the_fader");
    t(test_latch_holds_until_stop, "test_latch_holds_until_stop");
    t(test_forgotten_latches_play_the_lane, "test_forgotten_latches_play_the_lane");
    t(test_write_writes_every_parameter_and_splits_at_the_wrap,
      "test_write_writes_every_parameter_and_splits_at_the_wrap");
    t(test_a_full_ring_is_counted, "test_a_full_ring_is_counted");
    if (failed != 0) {
        std::fprintf(stderr, "automation: %d of %d failed (heap counter: %s)\n", failed, run,
                     heap_counter_scope());
        return 1;
    }
    std::printf("automation: %d passed (heap counter: %s)\n", run, heap_counter_scope());
    return 0;
}
