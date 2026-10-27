// SPDX-License-Identifier: GPL-3.0-or-later
//
// Region fades as Session::process() plays them (engine.timeline.fades):
// each shape the model defines, in and out, with region gain. A fade's gain
// is a closed form of the timeline frame (session.cpp, segment_gain), so
// the output may not depend on how the backend cuts the roll into blocks,
// on a locate into the middle of a fade, or on a loop wrapping from inside
// a fade out back into a fade in. Nothing here touches the heap in
// process().
//
// The source's frame f is source_value(f), distinct per frame, so a sample
// from the wrong place is a wrong value.
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
using sonora::FadeCurve;
using sonora::Session;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kSource = 9;
constexpr int64_t kSourceFrames = 40 * kChunk;
constexpr FadeCurve kShapes[] = {FadeCurve::Linear, FadeCurve::EqualPower, FadeCurve::SCurve};

// Relative: source values run to thousands, where a float's step is ~5e-4.
bool near(float a, float b) {
    const float scale = std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f;
    return std::fabs(a - b) <= 1e-6f * scale;
}

// What the region plays at timeline frame t, from the fade law alone.
float expected(const SegmentSpec& sp, int64_t t) {
    if (t < sp.start || t >= sp.end) return 0.0f;
    float g = sp.gain;
    if (t < sp.start + sp.fade_in) {
        g *= sonora::fade_in_gain(sp.fade_in_shape, static_cast<float>(t - sp.start) /
                                                        static_cast<float>(sp.fade_in));
    }
    const int64_t out_start = sp.end - sp.fade_out;
    if (t >= out_start) {
        g *= sonora::fade_in_gain(sp.fade_out_shape,
                                  1.0f - static_cast<float>(t - out_start) /
                                             static_cast<float>(sp.fade_out));
    }
    return source_value(kSource, 0, sp.source_offset + (t - sp.start)) * g;
}

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    const float* in_ptrs[1];
    std::vector<float> out[2];
    float* out_ptrs[2];
    SegmentSpec region;

    Rig(const std::string& name, uint32_t max_block) {
        dir = fs::temp_directory_path() / ("sonora_test_region_fades_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = 1;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 128;
        cfg.playback.max_block_frames = max_block;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(max_block, 0.0f);
        in_ptrs[0] = in.data();
        for (int o = 0; o < 2; ++o) {
            out[o].assign(max_block, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        session->tracks()[0]->set_rec_armed(false);
        // Mono, hard left: out[0] is the region times exactly 1.
        session->tracks()[0]->set_pan(-1.0f);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    bool load(const SegmentSpec& sp) {
        Session& s = *session;
        const std::string path = (dir / "source.wav").string();
        if (!write_source(path, kSource, 1, kSourceFrames)) return false;
        uint32_t slot = 0;
        std::string error;
        if (!s.reader().add_source(path, slot, error)) return false;
        region = sp;
        region.source_slot = slot;
        if (!publish_plan(s, make_plan(s.plans().next_epoch(), 1, {region}, kChunk))) return false;
        s.reader().run_pass();
        // The faders ramp up from silence in the session's first block.
        for (int b = 0; b < 2; ++b) block(64);
        return true;
    }

    bool loop(uint64_t a, uint64_t b) {
        Session& s = *session;
        std::string error;
        if (!s.set_loop(a, b, &error)) {
            std::fprintf(stderr, "  set_loop: %s\n", error.c_str());
            return false;
        }
        for (int k = 0; k < 8 && s.loop_pending(); ++k) {
            s.reader().run_pass();
            s.pump_loop();
        }
        return !s.loop_pending();
    }

    // Stopped: the locate lands in the next block, once its window is in.
    bool locate(uint64_t frame) {
        Session& s = *session;
        if (!s.post_locate(frame)) return false;
        s.reader().run_pass();
        block(64);
        s.reader().run_pass();
        return s.transport().playhead() == frame;
    }

    void block(uint32_t n) {
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
    }

    // Rolls `frames` frames in blocks cycling through `sizes`, appending
    // out[0], with a reader pass after each block as real time gives it.
    void roll(int64_t frames, const std::vector<uint32_t>& sizes, std::vector<float>& got) {
        std::size_t k = 0;
        while (frames > 0) {
            const uint32_t n = static_cast<uint32_t>(
                frames < static_cast<int64_t>(sizes[k]) ? frames : sizes[k]);
            block(n);
            got.insert(got.end(), out[0].begin(), out[0].begin() + n);
            session->reader().run_pass();
            frames -= n;
            k = (k + 1) % sizes.size();
        }
    }
};

SegmentSpec faded_region(int64_t start, int64_t end, int64_t fade_in, int64_t fade_out,
                         FadeCurve in, FadeCurve out) {
    SegmentSpec sp;
    sp.start = start;
    sp.end = end;
    sp.source_offset = 37;
    sp.gain = 0.5f;
    sp.fade_in = fade_in;
    sp.fade_out = fade_out;
    sp.fade_in_shape = in;
    sp.fade_out_shape = out;
    return sp;
}

// The whole region, silence either side, in one block and then in blocks
// of every awkward size: the same samples, bit for bit, and the fade law's.
int test_block_split_invariance() {
    constexpr uint32_t kOne = 4096;
    for (FadeCurve in : kShapes) {
        for (FadeCurve out : kShapes) {
            std::vector<float> whole;
            std::vector<float> split;
            SegmentSpec sp = faded_region(200, 3800, 1000, 1500, in, out);
            {
                Rig rig("whole", kOne);
                CHECK(rig.load(sp));
                CHECK(rig.locate(0));
                reset_heap_counts();
                rig.session->play();
                rig.roll(kOne, {kOne}, whole);
                CHECK(heap_untouched());
                CHECK(rig.session->playback_misses() == 0);
                sp = rig.region;
            }
            {
                Rig rig("split", kOne);
                CHECK(rig.load(sp));
                CHECK(rig.locate(0));
                reset_heap_counts();
                rig.session->play();
                rig.roll(kOne, {1, 2, 3, 7, 61, 97, 255, 256, 257, 999}, split);
                CHECK(heap_untouched());
                CHECK(rig.session->playback_misses() == 0);
            }
            CHECK(whole.size() == kOne && split.size() == kOne);
            for (int64_t t = 0; t < kOne; ++t) {
                const auto i = static_cast<std::size_t>(t);
                if (whole[i] != split[i] || !near(whole[i], expected(sp, t))) {
                    std::fprintf(stderr, "  shapes %d/%d frame %lld: whole %.7f split %.7f want %.7f\n",
                                 static_cast<int>(in), static_cast<int>(out),
                                 static_cast<long long>(t), static_cast<double>(whole[i]),
                                 static_cast<double>(split[i]),
                                 static_cast<double>(expected(sp, t)));
                    return 1;
                }
            }
            // From silence, to unity at the fade's end, and back to one step
            // above silence on the region's last frame.
            CHECK(whole[199] == 0.0f && whole[200] == 0.0f);
            CHECK(whole[1200] == source_value(kSource, 0, 37 + 1000) * 0.5f);
            CHECK(whole[3799] > 0.0f && whole[3800] == 0.0f);
        }
    }
    return 0;
}

// A locate into the middle of a fade in, and of a fade out, plays the fade
// from where it stands at that frame, not from its start.
int test_locate_inside_a_fade() {
    constexpr uint32_t kBlock = 97;
    for (FadeCurve shape : kShapes) {
        Rig rig("locate", 256);
        CHECK(rig.load(faded_region(1000, 9000, 2000, 3000, shape, shape)));
        Session& s = *rig.session;
        reset_heap_counts();
        for (const int64_t at : {int64_t{1001}, int64_t{1777}, int64_t{2999}, int64_t{6000},
                                 int64_t{7654}, int64_t{8990}}) {
            CHECK(rig.locate(static_cast<uint64_t>(at)));
            s.play();
            std::vector<float> got;
            rig.roll(10 * kBlock, {kBlock}, got);
            s.stop_playback();
            for (int64_t i = 0; i < 10 * kBlock; ++i) {
                const float want = expected(rig.region, at + i);
                if (!near(got[static_cast<std::size_t>(i)], want)) {
                    std::fprintf(stderr, "  shape %d, located at %lld, frame %lld: %.7f, want %.7f\n",
                                 static_cast<int>(shape), static_cast<long long>(at),
                                 static_cast<long long>(at + i),
                                 static_cast<double>(got[static_cast<std::size_t>(i)]),
                                 static_cast<double>(want));
                    return 1;
                }
            }
        }
        CHECK(heap_untouched());
        CHECK(s.playback_misses() == 0);
    }
    return 0;
}

// A loop from inside the fade in to inside the fade out: at B the fade out
// stops where it stands and the next sample is the fade in's at A, through
// every pass, at any offset in a block.
int test_loop_wrap_inside_fades() {
    constexpr uint32_t kBlock = 97;
    constexpr int64_t kA = 2500;
    constexpr int64_t kB = 7000;
    for (FadeCurve shape : kShapes) {
        Rig rig("loop", 256);
        CHECK(rig.load(faded_region(1000, 9000, 2000, 3000, shape, FadeCurve::EqualPower)));
        Session& s = *rig.session;
        CHECK(rig.loop(kA, kB));
        CHECK(rig.locate(2000));
        reset_heap_counts();
        s.play();
        std::vector<float> got;
        const int64_t frames = (kB - 2000) + 3 * (kB - kA) + 123;
        rig.roll(frames, {kBlock}, got);
        CHECK(heap_untouched());
        CHECK(s.playback_misses() == 0);
        int64_t t = 2000;
        for (int64_t i = 0; i < frames; ++i) {
            const float want = expected(rig.region, t);
            if (!near(got[static_cast<std::size_t>(i)], want)) {
                std::fprintf(stderr, "  shape %d, output %lld (timeline %lld): %.7f, want %.7f\n",
                             static_cast<int>(shape), static_cast<long long>(i),
                             static_cast<long long>(t),
                             static_cast<double>(got[static_cast<std::size_t>(i)]),
                             static_cast<double>(want));
                return 1;
            }
            if (++t == kB) t = kA;
        }
        // The wrap lands inside a block, not on its edge.
        CHECK((kB - 2000) % kBlock != 0);
        s.stop_playback();
    }
    return 0;
}

// expected() is built on fade_in_gain, the function the engine plays, so a
// wrong law would pass every test above. These gains are typed in, worked by
// hand at x = i / 4 of a four-frame fade: linear x, equal power
// sin(pi/2 * x), s-curve (1 - cos(pi * x)) / 2. A fade out's frame i is at
// x = 1 - i / 4.
int test_fade_gains_by_hand() {
    struct Literal {
        FadeCurve shape;
        float at_quarter;
        float at_half;
        float at_three_quarters;
    };
    const Literal table[] = {
        {FadeCurve::Linear, 0.25f, 0.5f, 0.75f},
        {FadeCurve::EqualPower, 0.38268343f, 0.70710678f, 0.92387953f},
        {FadeCurve::SCurve, 0.14644661f, 0.5f, 0.85355339f},
    };
    constexpr int64_t kStart = 200;
    constexpr int64_t kEnd = 300;
    for (const Literal& lit : table) {
        Rig rig("literal", 512);
        CHECK(rig.load(faded_region(kStart, kEnd, 4, 4, lit.shape, lit.shape)));
        CHECK(rig.locate(0));
        rig.session->play();
        std::vector<float> got;
        rig.roll(512, {512}, got);
        rig.session->stop_playback();
        const struct {
            int64_t t;
            float gain;
        } samples[] = {
            {kStart, 0.0f},                       // fade in, x = 0
            {kStart + 2, lit.at_half},            // fade in, x = 2/4
            {kStart + 3, lit.at_three_quarters},  // fade in's last frame, x = 3/4
            {kEnd - 4, 1.0f},                     // fade out's first frame, x = 1
            {kEnd - 2, lit.at_half},              // fade out, x = 2/4
            {kEnd - 1, lit.at_quarter},           // region's last frame, x = 1/4
        };
        for (const auto& sm : samples) {
            // Region gain 0.5 on the source frame under t.
            const float want = source_value(kSource, 0, 37 + (sm.t - kStart)) * 0.5f * sm.gain;
            const float g = got[static_cast<std::size_t>(sm.t)];
            // Looser than near(): the literals carry eight digits.
            if (std::fabs(g - want) > 1e-5f * std::fabs(want) + 1e-6f) {
                std::fprintf(stderr, "  shape %d, frame %lld: %.7f, want %.7f\n",
                             static_cast<int>(lit.shape), static_cast<long long>(sm.t),
                             static_cast<double>(g), static_cast<double>(want));
                return 1;
            }
        }
    }
    return 0;
}

}  // namespace

int main() {
    if (test_block_split_invariance()) return 1;
    if (test_locate_inside_a_fade()) return 1;
    if (test_loop_wrap_inside_fades()) return 1;
    if (test_fade_gains_by_hand()) return 1;
    std::printf("region_fades: ok (heap counter: %s)\n", heap_counter_scope());
    return 0;
}
