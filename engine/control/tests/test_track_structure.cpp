// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track structure through PlaybackController (ADR-0022 §4): a reorder, a
// duplicate and a delete while rolling are heard exactly as the document
// says, before the new plan swaps in, at the swap and after it, with no
// allocation or free in Session::process() throughout; and the record arm
// and the capture input stay with their document track.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"

#define CHECK_APPLY(history, op)                                              \
    do {                                                                      \
        std::string check_apply_error_;                                       \
        if (!(history).apply((op), check_apply_error_)) {                     \
            std::fprintf(stderr, "FAIL %s:%d: apply refused: %s\n", __FILE__, \
                         __LINE__, check_apply_error_.c_str());               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace {

using namespace sonora::model;
using playback_test::source_value;
using sonora::PlaybackController;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
constexpr int64_t kLength = 80 * kChunk;
const std::string kDir = "test_track_structure_session";

std::unique_ptr<Session> make_session(int tracks) {
    sonora::SessionConfig cfg;
    cfg.dir = kDir;
    cfg.track_count = tracks;
    cfg.ring_seconds = 0.1;
    cfg.playback.chunk_frames = kChunk;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 128;
    cfg.playback.max_block_frames = 256;
    cfg.playback.max_segments = 64;
    auto s = std::make_unique<Session>(cfg);
    s->create_dirs();
    return s;
}

bool write_source(Id id) {
    return playback_test::write_source(kDir + "/Audio Files/src_" + std::to_string(id) + ".wav",
                                       static_cast<uint32_t>(id), 1, kLength);
}

AddSource source_op(Id id) {
    AddSource op;
    op.source.id = id;
    op.source.path = "Audio Files/src_" + std::to_string(id) + ".wav";
    op.source.length = kLength;
    op.source.format = AudioFormat::Float32;
    op.source.kind = SourceKind::Imported;
    return op;
}

AddTrack track_op(Id track, const std::string& name, float pan, uint16_t input) {
    AddTrack op;
    op.track = track;
    op.playlist = track + 1;
    op.name = name;
    op.playlist_name = name + ".01";
    op.mix.input = {input};
    op.mix.pan = pan;
    return op;
}

Region region(Id id, Id source) {
    Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.length = kLength;
    r.channels.push_back({source, 0});
    return r;
}

struct Out {
    std::vector<float> l = std::vector<float>(kBlock);
    std::vector<float> r = std::vector<float>(kBlock);
};

// One block, every allocation and free on this thread inside it counted.
void block(Session& s, Out& o) {
    float* outs[2] = {o.l.data(), o.r.data()};
    sonora::backend::AudioBlock b{};
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = kBlock;
    playback_test::t_counting = true;
    s.process(b);
    playback_test::t_counting = false;
}

// Left is `left` times source 1 (track A, hard left); right is source 2
// (track B, hard right) or silence.
bool heard(Session& s, Out& o, float left, bool right) {
    const int64_t at = static_cast<int64_t>(s.transport().playhead());
    block(s, o);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float l = left * source_value(1, 0, at + i);
        const float r = right ? source_value(2, 0, at + i) : 0.0f;
        if (o.l[i] != l || o.r[i] != r) {
            std::fprintf(stderr, "  frame %lld: L %f R %f, want %f %f\n",
                         static_cast<long long>(at + i), o.l[i], o.r[i], l, r);
            return false;
        }
    }
    return true;
}

// As EngineControlServer after an update: the mix, then the plan.
bool show(PlaybackController& ctl, const Snapshot& snapshot) {
    std::string error;
    ctl.apply_mix(snapshot);
    if (ctl.show(snapshot, error)) return true;
    std::fprintf(stderr, "  show: %s\n", error.c_str());
    return false;
}

// The document changed its tracks: the old plan plays on, exactly, until
// the new one is resident and published; from the block after that, the
// new one, exactly. Before and after are what each says is heard.
bool move(PlaybackController& ctl, Session& s, Out& o, const Snapshot& next,
          float left_before, bool right_before, float left_after, bool right_after) {
    if (!show(ctl, next) || !ctl.tracks_moving()) return false;
    for (int blk = 0; blk < 4; ++blk) {
        if (!heard(s, o, left_before, right_before)) return false;
    }
    bool swapped = false;
    for (int i = 0; i < 64 && (ctl.tracks_moving() || !swapped); ++i) {
        const uint64_t published = ctl.plans_published();
        s.reader().run_pass();
        ctl.pump();
        swapped = swapped || ctl.plans_published() != published;
        if (!heard(s, o, swapped ? left_after : left_before,
                   swapped ? right_after : right_before)) {
            return false;
        }
    }
    return swapped && !ctl.tracks_moving();
}

// settle(), then blocks until the moved tracks' mix is in the atomics: it
// waits for the audio thread to pass the old plan (#162 review B1).
bool settle_moved(PlaybackController& ctl, Session& s) {
    if (!ctl.settle(2.0)) return false;
    Out o;
    for (int i = 0; i < 8 && ctl.tracks_moving(); ++i) {
        block(s, o);
        ctl.pump();
    }
    return !ctl.tracks_moving();
}

int test_moves_are_heard_exactly_and_allocate_nothing() {
    auto session = make_session(4);
    Session& s = *session;
    CHECK(write_source(1) && write_source(2));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1));
    CHECK_APPLY(h, source_op(2));
    CHECK_APPLY(h, track_op(100, "A", -1.0f, 0));
    CHECK_APPLY(h, track_op(102, "B", 1.0f, 1));
    CHECK_APPLY(h, (PlaceRegion{101, region(110, 1), 0}));
    CHECK_APPLY(h, (PlaceRegion{103, region(111, 2), 0}));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    Out o;
    block(s, o);  // the ramps settle from silence
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == TransportState::Rolling);
    playback_test::reset_heap_counts();
    for (int blk = 0; blk < 4; ++blk) CHECK(heard(s, o, 1.0f, true));

    // B over A: each keeps its pan through the swap.
    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(move(ctl, s, o, h.current(), 1.0f, true, 1.0f, true));
    CHECK(s.tracks()[0]->pan() == 1.0f && s.tracks()[1]->pan() == -1.0f);

    // A copy of A below it: A twice on the left from the swap on.
    DuplicateTrack d;
    d.source = 100;
    d.track = h.current().next_id;
    d.name = duplicate_track_name(h.current(), 100);
    CHECK_APPLY(h, d);
    CHECK(move(ctl, s, o, h.current(), 1.0f, true, 2.0f, true));

    // The copy goes, then B: A alone, moved from engine track 1 to 0.
    CHECK_APPLY(h, RemoveTrack{d.track});
    CHECK(move(ctl, s, o, h.current(), 2.0f, true, 1.0f, true));
    CHECK_APPLY(h, RemoveTrack{102});
    CHECK(move(ctl, s, o, h.current(), 1.0f, true, 1.0f, false));
    for (int blk = 0; blk < 4; ++blk) CHECK(heard(s, o, 1.0f, false));

    CHECK(playback_test::heap_untouched());
    CHECK(s.playback_misses() == 0);
    std::printf("  heap counter: %s\n", playback_test::heap_counter_scope());
    return 0;
}

// #162 review B1: a block that acquired the old plan just before the
// publish runs on after it, and that plan carries no mix; it reads the
// atomics. So they keep the old order until the audio thread has begun two
// blocks past the publish (PlanExchange's own rule for the old plan's
// last block), and tracks_moving() holds until then.
int test_atomics_wait_for_the_old_plans_last_block() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, "Kick", -1.0f, 0));
    CHECK_APPLY(h, track_op(102, "Vox", 1.0f, 1));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    Out o;
    block(s, o);
    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(show(ctl, h.current()));
    const uint64_t published = ctl.plans_published();
    for (int i = 0; i < 64 && ctl.plans_published() == published; ++i) {
        s.reader().run_pass();
        ctl.pump();
    }
    CHECK(ctl.plans_published() == published + 1);
    // Published, no block since: the old plan may still be in one.
    CHECK(ctl.tracks_moving());
    CHECK(s.tracks()[0]->pan() == -1.0f && s.tracks()[1]->pan() == 1.0f);
    block(s, o);
    ctl.pump();
    CHECK(ctl.tracks_moving());
    CHECK(s.tracks()[0]->pan() == -1.0f);
    block(s, o);
    ctl.pump();
    CHECK(!ctl.tracks_moving());
    CHECK(s.tracks()[0]->pan() == 1.0f && s.tracks()[1]->pan() == -1.0f);
    return 0;
}

// Stopped, with nothing rolling: the arm and the input stay with their
// document track, and only once the plan that moves it is published.
int test_arm_and_input_follow_the_track() {
    auto session = make_session(3);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, "A", 0.0f, 0));
    CHECK_APPLY(h, track_op(102, "B", 0.0f, 1));
    CHECK_APPLY(h, track_op(104, "C", 0.0f, 2));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    s.tracks()[0]->set_rec_armed(true);

    CHECK_APPLY(h, (ReorderTracks{{102, 100, 104}}));
    CHECK(show(ctl, h.current()));
    CHECK(ctl.tracks_moving());
    CHECK(s.tracks()[0]->rec_armed() && s.tracks()[0]->input() == 0);
    CHECK(settle_moved(ctl, s));
    CHECK(!s.tracks()[0]->rec_armed() && s.tracks()[1]->rec_armed());
    CHECK(s.tracks()[0]->input() == 1 && s.tracks()[1]->input() == 0);
    CHECK(s.tracks()[2]->input() == 2);

    CHECK_APPLY(h, RemoveTrack{102});
    CHECK(show(ctl, h.current()));
    CHECK(settle_moved(ctl, s));
    CHECK(s.tracks()[0]->rec_armed() && s.tracks()[0]->input() == 0);
    CHECK(!s.tracks()[1]->rec_armed() && s.tracks()[1]->input() == 2);
    // No document track on it: its own input, disarmed.
    CHECK(!s.tracks()[2]->rec_armed() && s.tracks()[2]->input() == 2);

    DuplicateTrack d;
    d.source = 100;
    d.track = h.current().next_id;
    d.name = "A copy";
    CHECK_APPLY(h, d);
    CHECK(show(ctl, h.current()));
    CHECK(settle_moved(ctl, s));
    CHECK(s.tracks()[0]->rec_armed() && !s.tracks()[1]->rec_armed());
    CHECK(s.tracks()[1]->input() == 0 && s.tracks()[2]->input() == 2);
    return 0;
}

}  // namespace

int main() {
    if (test_moves_are_heard_exactly_and_allocate_nothing() != 0) return 1;
    if (test_arm_and_input_follow_the_track() != 0) return 1;
    if (test_atomics_wait_for_the_old_plans_last_block() != 0) return 1;
    std::printf("test_track_structure: ok\n");
    return 0;
}
