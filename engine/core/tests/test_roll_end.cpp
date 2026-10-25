// SPDX-License-Identifier: GPL-3.0-or-later
//
// Post-roll on the audio thread (Session::set_roll_end()): a roll stops at
// its end frame, at the sample inside a block, with the playhead left on
// that frame and nothing played past it; a take running across the frame
// is never stopped, and once it has punched out the transport stops at its
// next block without moving back; a roll with no end plays on. Nothing in
// it touches the heap. A playback loop the roll does not reach before its
// end leaves the end armed; one it wraps at first switches it off.
#define PLAYBACK_TEST_COUNT_HEAP 1

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "playback_test.h"
#include "sonora/session.h"

namespace {

using namespace playback_test;
using sonora::PlaybackPlan;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
// Prime, so no end below lands on a block boundary unless a test puts it
// there.
constexpr uint32_t kBlock = 61;

struct Rig {
    std::unique_ptr<Session> session;
    std::vector<std::string> files;
    std::vector<float> in = std::vector<float>(1024, 0.0f);
    const float* in_ptrs[1];
    std::vector<float> out[2];
    float* out_ptrs[2];

    Rig() {
        sonora::SessionConfig cfg;
        cfg.dir = "test_roll_end_unused";
        cfg.track_count = 1;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 96;
        cfg.playback.max_block_frames = 256;
        session = std::make_unique<Session>(cfg);
        in_ptrs[0] = in.data();
        for (int o = 0; o < 2; ++o) {
            out[o].assign(1024, 0.0f);
            out_ptrs[o] = out[o].data();
        }
    }
    ~Rig() {
        session.reset();
        for (const std::string& f : files) std::remove(f.c_str());
    }

    // A track playing source 5 from frame 0, hard left.
    bool load() {
        const std::string path = "test_roll_end_5.wav";
        if (!write_source(path, 5, 1, 40 * kChunk)) return false;
        files.push_back(path);
        uint32_t slot = 0;
        std::string error;
        if (!session->reader().add_source(path, slot, error)) return false;
        SegmentSpec sp;
        sp.end = 40 * kChunk;
        sp.source_slot = slot;
        PlaybackPlan* p = make_plan(session->plans().next_epoch(), 1, {sp}, kChunk);
        sonora::DiskReader& r = session->reader();
        const uint64_t gen = r.serve(session->plans().current(), p);
        r.run_pass();
        if (r.resident() != gen || !session->plans().publish(p)) return false;
        r.serve(p, nullptr);
        r.run_pass();
        session->tracks()[0]->set_pan(-1.0f);
        return true;
    }

    // Through the residency gate, as engine/control's pump does it.
    bool loop(uint64_t a, uint64_t b) {
        Session& s = *session;
        if (!s.set_loop(a, b)) return false;
        for (int k = 0; k < 8 && s.loop_pending(); ++k) {
            s.reader().run_pass();
            s.pump_loop();
        }
        return !s.loop_pending();
    }

    void block(uint32_t n = kBlock) {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs;
        b.input_count = 1;
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        for (uint32_t i = 0; i < n; ++i) out[0][i] = 0.9f;  // stale backend data
        t_counting = true;
        session->process(b);
        t_counting = false;
    }
};

float played(uint64_t frame) { return source_value(5, 0, static_cast<int64_t>(frame)); }

// The end inside a block: the frames before it play, the rest of the block
// is silence, the playhead stops on it and the end is used up.
int test_stops_at_the_sample() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    reset_heap_counts();
    const uint64_t end = 3 * kBlock + 17;
    s.set_roll_end(end);
    s.play();
    for (int i = 0; i < 3; ++i) rig.block();
    CHECK(s.transport().state() == TransportState::Rolling);
    rig.block();
    for (uint32_t i = 0; i < 17; ++i) CHECK(rig.out[0][i] == played(3 * kBlock + i));
    for (uint32_t i = 17; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == end);
    CHECK(s.roll_end() == Session::kNoRollEnd);
    // Stopped, it stays there.
    rig.block();
    CHECK(s.transport().playhead() == end);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    CHECK(heap_untouched());
    return 0;
}

// On a block boundary: the whole block before it plays, and the transport
// stops with the playhead on the boundary.
int test_stops_on_a_boundary() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    s.set_roll_end(2 * kBlock);
    s.play();
    rig.block();
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == played(kBlock + i));
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == 2 * kBlock);
    return 0;
}

// A take is never stopped by it; once the take has ended and the transport
// is back to Rolling past the end, the next block stops it where it is.
int test_a_take_runs_past_it() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    s.set_roll_end(kBlock + 5);
    s.transport().set_state(TransportState::Recording);
    for (int i = 0; i < 4; ++i) rig.block();
    CHECK(s.transport().state() == TransportState::Recording);
    CHECK(s.transport().playhead() == 4 * kBlock);
    s.transport().set_state(TransportState::Rolling);
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == 4 * kBlock);
    return 0;
}

// No end, or one cleared before it is reached: the roll plays on.
int test_no_end_plays_on() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    s.set_roll_end(kBlock + 5);
    s.set_roll_end(Session::kNoRollEnd);
    s.play();
    for (int i = 0; i < 4; ++i) rig.block();
    CHECK(s.transport().state() == TransportState::Rolling);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == played(3 * kBlock + i));
    return 0;
}

// Stopped by the control thread first: the end is not consumed by a block
// that does not roll, and a later roll stops at it.
int test_survives_a_stop() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    s.set_roll_end(3 * kBlock + 1);
    s.play();
    rig.block();
    s.stop_playback();
    rig.block();
    CHECK(s.roll_end() == 3 * kBlock + 1);
    s.play();
    for (int i = 0; i < 3; ++i) rig.block();
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == 3 * kBlock + 1);
    return 0;
}

// A loop set later on the timeline: the roll reaches its end long before
// the loop's start, so it stops there instead of running on into the loop
// and wrapping for ever.
int test_a_later_loop_keeps_it() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    CHECK(rig.loop(8 * kChunk, 12 * kChunk));
    reset_heap_counts();
    const uint64_t end = 3 * kBlock + 17;
    s.set_roll_end(end);
    s.play();
    for (int i = 0; i < 4; ++i) rig.block();
    for (uint32_t i = 0; i < 17; ++i) CHECK(rig.out[0][i] == played(3 * kBlock + i));
    for (uint32_t i = 17; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == end);
    CHECK(heap_untouched());
    return 0;
}

// The end on the loop's B: the roll stops there, with the playhead on B,
// not wrapped back to A.
int test_an_end_on_the_wrap_point() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    const uint64_t b = 6 * kBlock;
    CHECK(rig.loop(kBlock, b));
    s.set_roll_end(b);
    s.play();
    for (int i = 0; i < 6; ++i) rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == played(5 * kBlock + i));
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == b);
    return 0;
}

// An end past the loop's B: the roll wraps before it gets there, and while
// looping post-roll does not apply (ADR-0016), so it plays on.
int test_an_end_past_the_loop_loops() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    const uint64_t b = 6 * kBlock;
    CHECK(rig.loop(kBlock, b));
    s.set_roll_end(b + 1);
    s.play();
    for (int i = 0; i < 20; ++i) rig.block();
    CHECK(s.transport().state() == TransportState::Rolling);
    CHECK(s.transport().playhead() < b);
    CHECK(s.roll_end() == b + 1);
    return 0;
}

// A take that punched out past the end with a loop set later: the next
// block stops the transport where it is, loop or not.
int test_a_later_loop_after_a_take() {
    Rig rig;
    CHECK(rig.load());
    Session& s = *rig.session;
    CHECK(rig.loop(8 * kChunk, 12 * kChunk));
    s.set_roll_end(kBlock + 5);
    s.transport().set_state(TransportState::Recording);
    for (int i = 0; i < 4; ++i) rig.block();
    CHECK(s.transport().state() == TransportState::Recording);
    s.transport().set_state(TransportState::Rolling);
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f);
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(s.transport().playhead() == 4 * kBlock);
    return 0;
}

}  // namespace

int main() {
    if (test_stops_at_the_sample()) return 1;
    if (test_stops_on_a_boundary()) return 1;
    if (test_a_take_runs_past_it()) return 1;
    if (test_no_end_plays_on()) return 1;
    if (test_survives_a_stop()) return 1;
    if (test_a_later_loop_keeps_it()) return 1;
    if (test_an_end_on_the_wrap_point()) return 1;
    if (test_an_end_past_the_loop_loops()) return 1;
    if (test_a_later_loop_after_a_take()) return 1;
    std::printf("roll_end: ok; heap counter: %s\n", heap_counter_scope());
    return 0;
}
