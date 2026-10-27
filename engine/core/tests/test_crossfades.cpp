// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crossfades as Session::process() plays them (engine.timeline.crossfades).
// A crossfade reaches the audio thread as two segments that overlap by its
// length (plan_builder.cpp): the left one extended into its handle and
// fading out, the right one starting early in its handle and fading in,
// both with the crossfade's shape. Pre, post and centred placements, every
// shape, odd lengths. The output may not depend on how the backend cuts
// the roll into blocks, on a locate into the middle of the crossfade, or on
// a loop that wraps into or out of it; nothing touches the heap in
// process().
//
// The source's frame f is source_value(f), distinct per frame, so a sample
// from the wrong place in either handle is a wrong value.
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

// Left region [200, kCut) from source frame 37; right region [kCut, 7000).
constexpr int64_t kLeftStart = 200;
constexpr int64_t kLeftOffset = 37;
constexpr int64_t kCut = 3000;
constexpr int64_t kRightEnd = 7000;
// The right region continues the left one's source: a region split and
// crossfaded against itself, the correlated case.
constexpr int64_t kContinuous = kLeftOffset + (kCut - kLeftStart);
// Elsewhere in the source: the uncorrelated case.
constexpr int64_t kElsewhere = 5000;

enum class Placement { Pre, Post, Centre };
constexpr Placement kPlacements[] = {Placement::Pre, Placement::Post, Placement::Centre};

// A centred odd length puts the extra frame after the cut, as
// model::crossfade_extent does.
void extent(Placement p, int64_t length, int64_t& before, int64_t& after) {
    switch (p) {
        case Placement::Pre: before = length; after = 0; return;
        case Placement::Post: before = 0; after = length; return;
        case Placement::Centre: break;
    }
    before = length / 2;
    after = length - before;
}

struct Pair {
    SegmentSpec left, right;
};

// The two segments plan_builder.cpp makes of two butted regions and the
// crossfade between them.
Pair crossfade(Placement p, int64_t length, FadeCurve shape, int64_t right_offset,
               float right_gain = 1.0f) {
    int64_t before = 0, after = 0;
    extent(p, length, before, after);
    Pair x;
    x.left.start = kLeftStart;
    x.left.end = kCut + after;
    x.left.source_offset = kLeftOffset;
    x.left.fade_out = length;
    x.left.fade_out_shape = shape;
    x.right.start = kCut - before;
    x.right.end = kRightEnd;
    x.right.source_offset = right_offset - before;
    x.right.gain = right_gain;
    x.right.fade_in = length;
    x.right.fade_in_shape = shape;
    return x;
}

// Relative: source values run to thousands, where a float's step is ~5e-4.
bool near(float a, float b, float rel = 2e-6f) {
    const float scale = std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f;
    return std::fabs(a - b) <= rel * scale;
}

float gain_at(const SegmentSpec& sp, int64_t t) {
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
    return g;
}

// What the two regions play at timeline frame t, from the fade law alone,
// summed in the order the segments are rendered.
float expected(const Pair& x, int64_t t) {
    float acc = 0.0f;
    for (const SegmentSpec* sp : {&x.left, &x.right}) {
        if (t < sp->start || t >= sp->end) continue;
        acc += source_value(kSource, 0, sp->source_offset + (t - sp->start)) * gain_at(*sp, t);
    }
    return acc;
}

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    const float* in_ptrs[1];
    std::vector<float> out[2];
    float* out_ptrs[2];
    Pair pair;

    Rig(const std::string& name, uint32_t max_block) {
        dir = fs::temp_directory_path() / ("sonora_test_crossfades_" + name);
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
        // Mono, hard left: out[0] is the regions times exactly 1.
        session->tracks()[0]->set_pan(-1.0f);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    bool load(const Pair& x) {
        Session& s = *session;
        const std::string path = (dir / "source.wav").string();
        if (!write_source(path, kSource, 1, kSourceFrames)) return false;
        uint32_t slot = 0;
        std::string error;
        if (!s.reader().add_source(path, slot, error)) return false;
        pair = x;
        pair.left.source_slot = slot;
        pair.right.source_slot = slot;
        if (!publish_plan(s, make_plan(s.plans().next_epoch(), 1, {pair.left, pair.right}, kChunk))) {
            return false;
        }
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

const char* name(Placement p) {
    return p == Placement::Pre ? "pre" : p == Placement::Post ? "post" : "centre";
}

// From silence before the left region to past the right one, in one block
// and in blocks of every awkward size: the same samples, bit for bit, and
// the fade law's. An odd length, so the centred one is lopsided by a frame.
int test_block_split_invariance() {
    constexpr uint32_t kOne = 8192;
    constexpr int64_t kLength = 1001;
    for (FadeCurve shape : kShapes) {
        for (Placement place : kPlacements) {
            std::vector<float> whole;
            std::vector<float> split;
            Pair x = crossfade(place, kLength, shape, kElsewhere, 0.5f);
            {
                Rig rig("whole", kOne);
                CHECK(rig.load(x));
                CHECK(rig.locate(0));
                reset_heap_counts();
                rig.session->play();
                rig.roll(kOne, {kOne}, whole);
                CHECK(heap_untouched());
                CHECK(rig.session->playback_misses() == 0);
                x = rig.pair;
            }
            {
                Rig rig("split", kOne);
                CHECK(rig.load(x));
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
                if (whole[i] != split[i] || !near(whole[i], expected(x, t))) {
                    std::fprintf(stderr, "  shape %d %s frame %lld: whole %.7f split %.7f want %.7f\n",
                                 static_cast<int>(shape), name(place), static_cast<long long>(t),
                                 static_cast<double>(whole[i]), static_cast<double>(split[i]),
                                 static_cast<double>(expected(x, t)));
                    return 1;
                }
            }
            // Either side of the crossfade, one region alone at its gain.
            int64_t before = 0, after = 0;
            extent(place, kLength, before, after);
            const auto at = [&](int64_t t) { return whole[static_cast<std::size_t>(t)]; };
            CHECK(at(kCut - before - 1) == source_value(kSource, 0, kContinuous - before - 1));
            CHECK(at(kCut + after) == source_value(kSource, 0, kElsewhere + after) * 0.5f);
            CHECK(at(kLeftStart - 1) == 0.0f && at(kRightEnd) == 0.0f);
        }
    }
    return 0;
}

// The pairing heard: a region crossfaded against its own continuation
// (correlated) passes through an equal-gain crossfade unchanged, and comes
// out +3 dB at the middle of an equal-power one, which is why equal power
// is for uncorrelated material only.
int test_correlated_material() {
    constexpr uint32_t kBlock = 128;
    constexpr int64_t kLength = 512;
    for (FadeCurve shape : kShapes) {
        Rig rig("correlated", kBlock);
        CHECK(rig.load(crossfade(Placement::Centre, kLength, shape, kContinuous)));
        CHECK(rig.locate(0));
        reset_heap_counts();
        rig.session->play();
        std::vector<float> got;
        rig.roll(kRightEnd + 100, {kBlock}, got);
        CHECK(heap_untouched());
        for (int64_t t = kCut - kLength / 2; t < kCut + kLength / 2; ++t) {
            const float source = source_value(kSource, 0, kLeftOffset + (t - kLeftStart));
            const float heard = got[static_cast<std::size_t>(t)];
            if (shape == FadeCurve::EqualPower) {
                CHECK(heard >= source * (1.0f - 2e-6f));
            } else if (!near(heard, source, 4e-6f)) {
                std::fprintf(stderr, "  shape %d frame %lld: %.7f, the source is %.7f\n",
                             static_cast<int>(shape), static_cast<long long>(t),
                             static_cast<double>(heard), static_cast<double>(source));
                return 1;
            }
        }
        if (shape == FadeCurve::EqualPower) {
            const float source = source_value(kSource, 0, kContinuous);
            CHECK(near(got[static_cast<std::size_t>(kCut)], source * std::sqrt(2.0f), 1e-5f));
        }
    }
    return 0;
}

// A locate into the middle of the crossfade plays both sides from where
// they stand at that frame, not from the crossfade's start.
int test_locate_inside_a_crossfade() {
    constexpr uint32_t kBlock = 97;
    for (FadeCurve shape : kShapes) {
        Rig rig("locate", 256);
        CHECK(rig.load(crossfade(Placement::Centre, 2000, shape, kElsewhere)));
        Session& s = *rig.session;
        reset_heap_counts();
        for (const int64_t at : {int64_t{2000}, int64_t{2001}, int64_t{2777}, int64_t{2999},
                                 int64_t{3000}, int64_t{3456}, int64_t{3999}}) {
            CHECK(rig.locate(static_cast<uint64_t>(at)));
            s.play();
            std::vector<float> got;
            rig.roll(12 * kBlock, {kBlock}, got);
            s.stop_playback();
            for (int64_t i = 0; i < 12 * kBlock; ++i) {
                const float want = expected(rig.pair, at + i);
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

// A loop that starts inside the crossfade, and one that ends inside it: at
// B both sides stop where they stand and the next sample is A's, through
// every pass, at any offset in a block.
int test_loop_wrap_through_a_crossfade() {
    constexpr uint32_t kBlock = 97;
    struct Range {
        int64_t a, b, from;
    };
    // The crossfade runs over [2000, 4000).
    constexpr Range kRanges[] = {{2500, 6000, 2100}, {1000, 3333, 900}, {2200, 3700, 2150}};
    for (FadeCurve shape : kShapes) {
        for (const Range& r : kRanges) {
            Rig rig("loop", 256);
            CHECK(rig.load(crossfade(Placement::Centre, 2000, shape, kElsewhere)));
            Session& s = *rig.session;
            CHECK(rig.loop(static_cast<uint64_t>(r.a), static_cast<uint64_t>(r.b)));
            CHECK(rig.locate(static_cast<uint64_t>(r.from)));
            reset_heap_counts();
            s.play();
            std::vector<float> got;
            const int64_t frames = (r.b - r.from) + 3 * (r.b - r.a) + 123;
            rig.roll(frames, {kBlock}, got);
            CHECK(heap_untouched());
            CHECK(s.playback_misses() == 0);
            int64_t t = r.from;
            for (int64_t i = 0; i < frames; ++i) {
                const float want = expected(rig.pair, t);
                if (!near(got[static_cast<std::size_t>(i)], want)) {
                    std::fprintf(stderr, "  shape %d, loop %lld-%lld, output %lld (timeline %lld): "
                                         "%.7f, want %.7f\n",
                                 static_cast<int>(shape), static_cast<long long>(r.a),
                                 static_cast<long long>(r.b), static_cast<long long>(i),
                                 static_cast<long long>(t),
                                 static_cast<double>(got[static_cast<std::size_t>(i)]),
                                 static_cast<double>(want));
                    return 1;
                }
                if (++t == r.b) t = r.a;
            }
            // The wrap lands inside a block, not on its edge.
            CHECK((r.b - r.from) % kBlock != 0);
            s.stop_playback();
        }
    }
    return 0;
}

}  // namespace

int main() {
    if (test_block_split_invariance()) return 1;
    if (test_correlated_material()) return 1;
    if (test_locate_inside_a_crossfade()) return 1;
    if (test_loop_wrap_through_a_crossfade()) return 1;
    std::printf("crossfades: ok (heap counter: %s)\n", heap_counter_scope());
    return 0;
}
