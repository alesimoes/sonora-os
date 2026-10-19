// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Session::process() plays (timeline-model.md, "The monitor bus
// plan"; ADR-0009 §4.5, §4.6): region data summed into the 2-channel bus
// with region gain, fades and crossfades, track gain, pan, mute and solo
// ramped across the block, bus gain, the other outputs silent; a missing
// chunk is silence and counted; a locate sent once its window is resident
// plays the target from the next block; a plan swap and a delay line keep
// going across the swap; input monitoring by the track's mode.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "playback_test.h"
#include "sonora/session.h"

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace {

using namespace playback_test;
using sonora::FadeCurve;
using sonora::PlaybackPlan;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
const float kCentre = std::sqrt(0.5f);  // pan_exponent 0.5: -3.01 dB

// Relative: source values run to thousands, where a float's step is ~1e-4.
bool near(float a, float b, float tol = 1e-6f) {
    const float scale = std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f;
    return std::fabs(a - b) <= tol * scale;
}

struct Rig {
    std::unique_ptr<Session> session;
    std::vector<std::string> files;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[4];
    float* out_ptrs[4];

    Rig(int tracks, uint32_t max_delay = 0, uint32_t max_block = 256) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_playback_mix_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.02;  // 960 frames + a block
        cfg.playback.pool_chunks = 96;
        cfg.playback.max_block_frames = max_block;
        cfg.playback.max_delay_frames = max_delay;
        session = std::make_unique<Session>(cfg);
        in.assign(tracks, std::vector<float>(8192, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 4; ++o) {
            out[o].assign(8192, 0.0f);
            out_ptrs[o] = out[o].data();
        }
    }
    ~Rig() {
        session.reset();
        for (const std::string& f : files) std::remove(f.c_str());
    }

    // Source `id`, 32-bit float, `frames` long; returns its table slot.
    uint32_t source(uint32_t id, uint16_t channels, int64_t frames) {
        const std::string path = "test_playback_mix_" + std::to_string(id) + ".wav";
        write_source(path, id, channels, frames);
        files.push_back(path);
        uint32_t slot = 0;
        std::string error;
        session->reader().add_source(path, slot, error);
        return slot;
    }

    // The residency gate, synchronously: serve as pending until resident,
    // then exchange and serve as playing.
    bool publish(PlaybackPlan* p) {
        sonora::DiskReader& r = session->reader();
        PlaybackPlan* cur = session->plans().current();
        const uint64_t gen = r.serve(cur, p);
        r.run_pass();
        if (r.resident() != gen) return false;
        if (!session->plans().publish(p)) return false;
        r.serve(p, nullptr);
        r.run_pass();
        return true;
    }

    PlaybackPlan* plan(uint32_t tracks, const std::vector<SegmentSpec>& specs, uint32_t delay = 0) {
        return make_plan(session->plans().next_epoch(), tracks, specs, kChunk, delay);
    }

    void block(uint32_t n, uint32_t outputs = 2) {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = outputs;
        b.nframes = n;
        for (uint32_t o = 0; o < outputs; ++o) {
            for (uint32_t i = 0; i < n; ++i) out[o][i] = 0.9f;  // stale backend data
        }
        session->process(b);
    }
};

int test_fade_curves() {
    for (int i = 0; i <= 100; ++i) {
        const float x = static_cast<float>(i) / 100.0f;
        const float lin = sonora::fade_in_gain(FadeCurve::Linear, x) +
                          sonora::fade_in_gain(FadeCurve::Linear, 1.0f - x);
        const float ep_in = sonora::fade_in_gain(FadeCurve::EqualPower, x);
        const float ep_out = sonora::fade_in_gain(FadeCurve::EqualPower, 1.0f - x);
        const float sc = sonora::fade_in_gain(FadeCurve::SCurve, x) +
                         sonora::fade_in_gain(FadeCurve::SCurve, 1.0f - x);
        CHECK(near(lin, 1.0f));
        CHECK(near(ep_in * ep_in + ep_out * ep_out, 1.0f, 2e-6f));
        CHECK(near(sc, 1.0f, 2e-6f));
    }
    CHECK(sonora::fade_in_gain(FadeCurve::EqualPower, 0.0f) == 0.0f);
    CHECK(sonora::fade_in_gain(FadeCurve::EqualPower, 1.0f) == 1.0f);
    CHECK(near(sonora::fade_in_gain(FadeCurve::EqualPower, 0.5f), kCentre));
    CHECK(near(sonora::fade_in_gain(FadeCurve::SCurve, 0.25f), 0.5f - 0.5f * std::cos(0.25f * 3.14159265f)));
    // The law itself, by value, away from the symmetry points: the checks
    // above also pass for sqrt(x) as "equal power" or smoothstep as the
    // s-curve. ui/qt draws through this same function (sonora/fade_curve.h),
    // so a change here is a change to what the timeline shows.
    struct Pin {
        FadeCurve shape;
        float x, gain;
    };
    const Pin pins[] = {
        {FadeCurve::Linear, 0.1f, 0.1f},           {FadeCurve::Linear, 0.8f, 0.8f},
        {FadeCurve::EqualPower, 0.1f, 0.15643447f}, {FadeCurve::EqualPower, 0.25f, 0.38268343f},
        {FadeCurve::EqualPower, 0.8f, 0.95105652f}, {FadeCurve::SCurve, 0.1f, 0.02447174f},
        {FadeCurve::SCurve, 0.25f, 0.14644661f},    {FadeCurve::SCurve, 0.8f, 0.90450850f},
    };
    for (const Pin& p : pins) CHECK(near(sonora::fade_in_gain(p.shape, p.x), p.gain, 1e-6f));
    return 0;
}

// Two mono tracks panned hard apart: each side is exactly its region's
// source; panned centre, both sides are the sum at the pan law.
int test_sum_of_known_signals() {
    Rig rig(2);
    Session& s = *rig.session;
    const uint32_t a = rig.source(1, 1, 8 * kChunk);
    const uint32_t b = rig.source(2, 1, 8 * kChunk);
    SegmentSpec sa;
    sa.track = 0;
    sa.start = 100;
    sa.end = 100 + 6 * kChunk;
    sa.source_offset = 50;
    sa.source_slot = a;
    SegmentSpec sb = sa;
    sb.track = 1;
    sb.start = 0;
    sb.end = 6 * kChunk;
    sb.source_offset = 0;
    sb.source_slot = b;
    CHECK(rig.publish(rig.plan(2, {sa, sb})));
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_pan(1.0f);
    rig.block(kBlock);  // the ramps settle from silence at start-up
    s.play();
    for (int blk = 0; blk < 10; ++blk) {
        const uint64_t at = s.transport().playhead();
        rig.block(kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const int64_t t = static_cast<int64_t>(at + i);
            const float want_l = t >= 100 ? source_value(1, 0, 50 + t - 100) : 0.0f;
            CHECK(rig.out[0][i] == want_l);
            CHECK(rig.out[1][i] == source_value(2, 0, t));
        }
    }
    // Centre: ramped over one block, then exact.
    s.tracks()[0]->set_pan(0.0f);
    s.tracks()[1]->set_pan(0.0f);
    rig.block(kBlock);
    const uint64_t at = s.transport().playhead();
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const int64_t t = static_cast<int64_t>(at + i);
        const float sum = source_value(1, 0, 50 + t - 100) * kCentre + source_value(2, 0, t) * kCentre;
        CHECK(near(rig.out[0][i], sum, 1e-4f));
        CHECK(near(rig.out[1][i], sum, 1e-4f));
    }
    CHECK(s.playback_misses() == 0);
    return 0;
}

int test_region_gain_and_fades() {
    for (FadeCurve shape : {FadeCurve::Linear, FadeCurve::EqualPower, FadeCurve::SCurve}) {
        Rig rig(1);
        Session& s = *rig.session;
        const uint32_t src = rig.source(3, 1, 8 * kChunk);
        SegmentSpec sp;
        sp.start = 0;
        sp.end = 4 * kChunk;
        sp.source_slot = src;
        sp.gain = 0.5f;
        sp.fade_in = 100;
        sp.fade_out = 300;
        sp.fade_in_shape = shape;
        sp.fade_out_shape = shape;
        CHECK(rig.publish(rig.plan(1, {sp})));
        s.tracks()[0]->set_pan(-1.0f);
        rig.block(kBlock);
        s.transport().set_playhead(0);
        s.play();
        std::vector<float> got;
        while (s.transport().playhead() < static_cast<uint64_t>(sp.end) + kBlock) {
            rig.block(kBlock);
            got.insert(got.end(), rig.out[0].begin(), rig.out[0].begin() + kBlock);
        }
        for (int64_t t = 0; t < sp.end + kBlock; ++t) {
            float g = 0.0f;
            if (t < sp.end) {
                g = 0.5f;
                if (t < 100) g *= sonora::fade_in_gain(shape, static_cast<float>(t) / 100.0f);
                if (t >= sp.end - 300) {
                    g *= sonora::fade_in_gain(shape, 1.0f - static_cast<float>(t - (sp.end - 300)) / 300.0f);
                }
            }
            const float want = t < sp.end ? source_value(3, 0, t) * g : 0.0f;
            CHECK(near(got[static_cast<std::size_t>(t)], want, 1e-5f));
        }
        // The fade starts from silence and ends one step above it.
        CHECK(got[0] == 0.0f);
        CHECK(got[static_cast<std::size_t>(sp.end - 1)] > 0.0f);
    }
    return 0;
}

// A crossfade is two segments overlapping by its length, the left fading
// out and the right fading in over the same frames. With a source of
// constant level on both sides the shape shows directly: linear sums to the
// level, equal power to the level in power.
int test_crossfade() {
    for (FadeCurve shape : {FadeCurve::Linear, FadeCurve::EqualPower}) {
        Rig rig(1);
        Session& s = *rig.session;
        // Two channels of one source as the two sides, so each is a
        // distinct known signal.
        const uint32_t src = rig.source(4, 2, 8 * kChunk);
        const int64_t cut = 2 * kChunk + 10;
        const int64_t before = 64;
        const int64_t after = 64;
        SegmentSpec left;
        left.start = 0;
        left.end = cut + after;
        left.source_slot = src;
        left.fade_out = before + after;
        left.fade_out_shape = shape;
        SegmentSpec right;
        right.start = cut - before;
        right.end = 6 * kChunk;
        right.source_offset = right.start;
        right.source_slot = src;
        right.source_channel0 = 1;
        right.fade_in = before + after;
        right.fade_in_shape = shape;
        CHECK(rig.publish(rig.plan(1, {left, right})));
        s.tracks()[0]->set_pan(-1.0f);
        rig.block(kBlock);
        s.transport().set_playhead(0);
        s.play();
        std::vector<float> got;
        while (s.transport().playhead() < static_cast<uint64_t>(cut + 2 * kBlock)) {
            rig.block(kBlock);
            got.insert(got.end(), rig.out[0].begin(), rig.out[0].begin() + kBlock);
        }
        for (int64_t t = cut - before; t < cut + after; ++t) {
            const float x = static_cast<float>(t - (cut - before)) / static_cast<float>(before + after);
            const float gl = sonora::fade_in_gain(shape, 1.0f - x);
            const float gr = sonora::fade_in_gain(shape, x);
            if (shape == FadeCurve::Linear) {
                CHECK(near(gl + gr, 1.0f));
            } else {
                CHECK(near(gl * gl + gr * gr, 1.0f, 2e-6f));
            }
            const float want = source_value(4, 0, t) * gl + source_value(4, 1, t) * gr;
            CHECK(near(got[static_cast<std::size_t>(t)], want, 1e-4f));
        }
        // Outside the crossfade, one side alone at unity.
        CHECK(got[static_cast<std::size_t>(cut - before - 1)] == source_value(4, 0, cut - before - 1));
        CHECK(got[static_cast<std::size_t>(cut + after)] == source_value(4, 1, cut + after));
        CHECK(s.playback_misses() == 0);
    }
    return 0;
}

// Gain, mute and solo change between blocks: the next block ramps
// linearly from the old coefficient to the new one, never a step.
int test_per_block_ramps() {
    Rig rig(2);
    Session& s = *rig.session;
    const uint32_t a = rig.source(5, 1, 16 * kChunk);
    SegmentSpec sp;
    sp.end = 16 * kChunk;
    sp.source_slot = a;
    SegmentSpec sp2 = sp;
    sp2.track = 1;
    CHECK(rig.publish(rig.plan(2, {sp, sp2})));
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_pan(1.0f);
    s.play();
    rig.block(kBlock);
    const auto ramp = [&](int side, float from, float to) -> bool {
        const uint64_t at = s.transport().playhead();
        rig.block(kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float g = from + (to - from) * static_cast<float>(i + 1) / kBlock;
            if (!near(rig.out[side][i], source_value(5, 0, static_cast<int64_t>(at + i)) * g, 1e-4f)) {
                std::fprintf(stderr, "  frame %u: %f, want %f\n", i, rig.out[side][i],
                             source_value(5, 0, static_cast<int64_t>(at + i)) * g);
                return false;
            }
        }
        return true;
    };
    s.tracks()[0]->set_gain(0.25f);
    CHECK(ramp(0, 1.0f, 0.25f));
    CHECK(ramp(0, 0.25f, 0.25f));
    s.tracks()[0]->set_muted(true);
    CHECK(ramp(0, 0.25f, 0.0f));
    s.tracks()[0]->set_muted(false);
    CHECK(ramp(0, 0.0f, 0.25f));
    // Solo track 1: track 0 ramps to silence, track 1 is untouched.
    s.tracks()[1]->set_soloed(true);
    CHECK(ramp(0, 0.25f, 0.0f));
    s.tracks()[1]->set_soloed(false);
    CHECK(ramp(0, 0.0f, 0.25f));
    s.tracks()[1]->set_soloed(true);
    CHECK(ramp(1, 1.0f, 1.0f));
    // The bus gain ramps the same way, on both sides.
    s.tracks()[1]->set_soloed(false);
    rig.block(kBlock);
    s.set_bus_gain(0.5f);
    CHECK(ramp(1, 1.0f, 0.5f));
    CHECK(s.take_bus_peak(1) > 0.0f);
    CHECK(s.take_bus_peak(1) == 0.0f);
    return 0;
}

// A solo-safe track is never implicitly muted by another track's solo, and
// its own mute still silences it, as in Pro Tools.
int test_solo_safe() {
    Rig rig(2);
    Session& s = *rig.session;
    const uint32_t a = rig.source(5, 1, 16 * kChunk);
    SegmentSpec sp;
    sp.end = 16 * kChunk;
    sp.source_slot = a;
    SegmentSpec sp2 = sp;
    sp2.track = 1;
    CHECK(rig.publish(rig.plan(2, {sp, sp2})));
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_pan(1.0f);
    s.play();
    rig.block(kBlock);
    const auto level = [&](int side, float g) -> bool {
        const uint64_t at = s.transport().playhead();
        rig.block(kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = source_value(5, 0, static_cast<int64_t>(at + i)) * g;
            if (!near(rig.out[side][i], want, 1e-4f)) {
                std::fprintf(stderr, "  side %d frame %u: %f, want %f\n", side, i,
                             rig.out[side][i], want);
                return false;
            }
        }
        return true;
    };
    s.tracks()[0]->set_solo_safe(true);
    s.tracks()[1]->set_soloed(true);
    CHECK(level(0, 1.0f));
    CHECK(level(0, 1.0f));
    CHECK(level(1, 1.0f));
    s.tracks()[0]->set_muted(true);
    rig.block(kBlock);
    CHECK(level(0, 0.0f));
    s.tracks()[0]->set_muted(false);
    s.tracks()[0]->set_solo_safe(false);
    rig.block(kBlock);
    CHECK(level(0, 0.0f));
    // Solo safe on the only soloed track changes nothing: it is heard.
    s.tracks()[1]->set_solo_safe(true);
    CHECK(level(1, 1.0f));
    return 0;
}

// A slot with no chunk is silence and a counted miss; process() returns
// without waiting for the reader.
int test_miss_is_counted_silence() {
    Rig rig(1);
    Session& s = *rig.session;
    const uint32_t a = rig.source(6, 1, 8 * kChunk);
    SegmentSpec sp;
    sp.end = 8 * kChunk;
    sp.source_slot = a;
    PlaybackPlan* p = rig.plan(1, {sp});
    // Published without the gate, as the gate exists to prevent.
    CHECK(s.plans().publish(p));
    s.play();
    rig.block(kBlock);
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    CHECK(s.playback_misses() >= 2);
    return 0;
}

// The locate gate: the reader makes the target resident, then the locate
// goes to the audio thread, and the very next block plays the target.
int test_locate_without_dropping_a_block() {
    Rig rig(1);
    Session& s = *rig.session;
    const uint32_t a = rig.source(7, 1, 40 * kChunk);
    SegmentSpec sp;
    sp.end = 40 * kChunk;
    sp.source_slot = a;
    PlaybackPlan* p = rig.plan(1, {sp});
    CHECK(rig.publish(p));
    s.tracks()[0]->set_pan(-1.0f);
    rig.block(kBlock);
    s.transport().set_playhead(0);
    s.play();
    rig.block(kBlock);
    const uint64_t target = 30 * kChunk + 7;
    sonora::DiskReader& r = s.reader();
    const uint64_t gen = r.serve(p, nullptr, static_cast<int64_t>(target), 1);
    r.run_pass();
    CHECK(r.resident() == gen);
    CHECK(s.post_locate(target));
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        CHECK(rig.out[0][i] == source_value(7, 0, static_cast<int64_t>(target + i)));
    }
    CHECK(s.transport().playhead() == target + kBlock);
    CHECK(s.transport().locates_applied() == 1);
    CHECK(s.playback_misses() == 0);
    return 0;
}

// A swap to a plan of the same content mid-roll continues sample for
// sample, and a delay line keeps its contents across it.
int test_swap_and_delay_continuity() {
    Rig rig(1, 32);
    Session& s = *rig.session;
    const uint32_t a = rig.source(8, 1, 16 * kChunk);
    SegmentSpec sp;
    sp.end = 16 * kChunk;
    sp.source_slot = a;
    constexpr uint32_t kDelay = 10;
    CHECK(rig.publish(rig.plan(1, {sp}, kDelay)));
    s.tracks()[0]->set_pan(-1.0f);
    rig.block(kBlock);
    s.transport().set_playhead(0);
    s.play();
    std::vector<float> got;
    for (int blk = 0; blk < 12; ++blk) {
        if (blk == 5) CHECK(rig.publish(rig.plan(1, {sp}, kDelay)));
        rig.block(kBlock);
        got.insert(got.end(), rig.out[0].begin(), rig.out[0].begin() + kBlock);
    }
    for (uint32_t t = kDelay + 1; t < got.size(); ++t) {
        CHECK(got[t] == source_value(8, 0, static_cast<int64_t>(t - kDelay)));
    }
    return 0;
}

// Auto monitoring: an armed track hears its input while stopped, and its
// playback while rolling; Off hears nothing; Input always the input.
int test_input_monitoring() {
    Rig rig(1);
    Session& s = *rig.session;
    const uint32_t a = rig.source(9, 1, 8 * kChunk);
    SegmentSpec sp;
    sp.end = 8 * kChunk;
    sp.source_slot = a;
    CHECK(rig.publish(rig.plan(1, {sp})));
    for (uint32_t i = 0; i < kBlock; ++i) rig.in[0][i] = 0.25f + static_cast<float>(i) * 1e-3f;
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[0]->set_rec_armed(true);
    rig.block(kBlock);
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == rig.in[0][i]);
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Off);
    rig.block(kBlock);
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Auto);
    s.transport().set_playhead(0);
    s.play();
    rig.block(kBlock);
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        CHECK(rig.out[0][i] == source_value(9, 0, static_cast<int64_t>(kBlock + i)));
    }
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Input);
    rig.block(kBlock);
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == rig.in[0][i]);
    return 0;
}

// A track that sends its input does not render, so it does not see a
// locate. Back on playback it must look for its segment again, not resume
// from where its cursor stopped before the switch.
int test_cursor_after_monitor_switch() {
    Rig rig(1);
    Session& s = *rig.session;
    const uint32_t a = rig.source(11, 1, 8 * kChunk);
    SegmentSpec first;
    first.end = 300;
    first.source_slot = a;
    SegmentSpec second = first;
    second.start = 300;
    second.end = 1100;
    second.source_offset = 300;
    CHECK(rig.publish(rig.plan(1, {first, second})));
    s.tracks()[0]->set_pan(-1.0f);
    rig.block(kBlock);
    s.transport().set_playhead(0);
    s.play();
    for (int i = 0; i < 6; ++i) rig.block(kBlock);  // past the first segment
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Input);
    rig.block(kBlock);
    s.transport().set_playhead(0);
    rig.block(kBlock);
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Auto);
    const uint64_t at = s.transport().playhead();
    CHECK(at == kBlock);
    rig.block(kBlock);
    // The gain ramp is at unity on both sides of the switch; the source
    // ramps from the (silent) input to playback over monitor_ramp_frames,
    // and the playback in it is already the right frames.
    const uint32_t ramp = s.config().playback.monitor_ramp_frames;
    CHECK(ramp == kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float w = static_cast<float>(i + 1) / static_cast<float>(ramp);
        CHECK(near(rig.out[0][i], w * source_value(11, 0, static_cast<int64_t>(at + i))));
    }
    const uint64_t next = s.transport().playhead();
    rig.block(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
        CHECK(rig.out[0][i] == source_value(11, 0, static_cast<int64_t>(next + i)));
    }
    CHECK(s.playback_misses() == 0);
    return 0;
}

// The audio thread runs with denormals flushed: a level that decays below
// FLT_MIN comes out as zero, not as a subnormal that costs a hundred cycles
// per operation on x86.
int test_denormals_are_flushed() {
    Rig rig(1);
    Session& s = *rig.session;
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Input);
    s.tracks()[0]->set_pan(-1.0f);
    // 1e-20 x 1e-20 is 1e-40, below FLT_MIN (1.2e-38) and above zero.
    s.tracks()[0]->set_gain(1e-20f);
    for (uint32_t i = 0; i < kBlock; ++i) rig.in[0][i] = 1e-20f;
#if defined(__x86_64__) || defined(__i386__)
    // Under pw-jack the callback runs on PipeWire's data loop, shared with
    // other nodes that expect IEEE arithmetic: the mode is the block's
    // alone. FTZ is bit 15 of MXCSR, DAZ bit 6.
    const unsigned host_mode = _mm_getcsr() & 0x8040u;
    CHECK(host_mode != 0x8040u);
#endif
    rig.block(kBlock);
    rig.block(kBlock);
#if defined(__x86_64__) || defined(__i386__)
    CHECK((_mm_getcsr() & 0x8040u) == host_mode);
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
#endif
    return 0;
}

// A block larger than the session's mixing maximum is mixed in pieces,
// exactly, and every output the plan does not name is written silent.
int test_large_block_and_other_outputs() {
    Rig rig(1, 0, 256);
    Session& s = *rig.session;
    const uint32_t a = rig.source(10, 1, 32 * kChunk);
    SegmentSpec sp;
    sp.end = 32 * kChunk;
    sp.source_slot = a;
    CHECK(rig.publish(rig.plan(1, {sp})));
    s.tracks()[0]->set_pan(-1.0f);
    rig.block(kBlock, 4);
    s.transport().set_playhead(0);
    s.play();
    rig.block(700, 4);
    for (uint32_t i = 0; i < 700; ++i) {
        CHECK(rig.out[0][i] == source_value(10, 0, i));
        CHECK(rig.out[1][i] == 0.0f);
        CHECK(rig.out[2][i] == 0.0f);
        CHECK(rig.out[3][i] == 0.0f);
    }
    return 0;
}

}  // namespace

int main() {
    if (test_fade_curves()) return 1;
    if (test_sum_of_known_signals()) return 1;
    if (test_region_gain_and_fades()) return 1;
    if (test_crossfade()) return 1;
    if (test_per_block_ramps()) return 1;
    if (test_solo_safe()) return 1;
    if (test_miss_is_counted_silence()) return 1;
    if (test_locate_without_dropping_a_block()) return 1;
    if (test_swap_and_delay_continuity()) return 1;
    if (test_input_monitoring()) return 1;
    if (test_large_block_and_other_outputs()) return 1;
    if (test_cursor_after_monitor_switch()) return 1;
    if (test_denormals_are_flushed()) return 1;
    std::printf("playback_mix: ok\n");
    return 0;
}
