// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track structure through PlaybackController (ADR-0023 §4): a reorder, a
// duplicate and a delete while rolling are heard exactly as the document
// says, before the new plan swaps in, at the swap and after it, with no
// allocation or free in Session::process() throughout; and the record arm
// and the capture input stay with their document track.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cmath>
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

// Left is `left` times source 1 (track A, hard left); right is `right`
// times source 2 (track B, hard right): exactly, or within `tol` where
// more than two copies sum and the order of the sum rounds.
bool heard(Session& s, Out& o, float left, float right, float tol = 0.0f) {
    const int64_t at = static_cast<int64_t>(s.transport().playhead());
    block(s, o);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float l = left * source_value(1, 0, at + i);
        const float r = right * source_value(2, 0, at + i);
        if (std::fabs(o.l[i] - l) > tol || std::fabs(o.r[i] - r) > tol) {
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
          float left_before, float right_before, float left_after, float right_after,
          float tol = 0.0f) {
    if (!show(ctl, next) || !ctl.tracks_moving()) return false;
    for (int blk = 0; blk < 4; ++blk) {
        if (!heard(s, o, left_before, right_before, tol)) return false;
    }
    bool swapped = false;
    for (int i = 0; i < 64 && (ctl.tracks_moving() || !swapped); ++i) {
        const uint64_t published = ctl.plans_published();
        s.reader().run_pass();
        ctl.pump();
        swapped = swapped || ctl.plans_published() != published;
        if (!heard(s, o, swapped ? left_after : left_before,
                   swapped ? right_after : right_before, tol)) {
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

// Sends and aux buses (ADR-0022) under a move. A sends to Verb from slot
// 0 and B from slot 1, post-fader, panning with their tracks, so the bus
// returns A on the left and B on the right: 2A and 2B. The engine keys a
// send's state by row and slot; a reorder, a duplicate and deletes that
// move rows keep each send on its own track through every swap, and a
// deleted track's send stops feeding the bus at its swap. Nothing is
// allocated in process().
int test_sends_move_with_their_tracks() {
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
    CHECK_APPLY(h, (AddBus{200, "Verb", BusMix{}}));
    SendMix unity;
    unity.level_db = 0.0f;
    CHECK_APPLY(h, (AddSend{100, 201, 0, 200, unity}));
    CHECK_APPLY(h, (AddSend{102, 202, 1, 200, unity}));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    Out o;
    ctl.play();
    CHECK(ctl.settle(2.0));
    for (int blk = 0; blk < 3; ++blk) block(s, o);   // the sends ramp in
    playback_test::reset_heap_counts();
    for (int blk = 0; blk < 4; ++blk) CHECK(heard(s, o, 2.0f, 2.0f));

    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(move(ctl, s, o, h.current(), 2.0f, 2.0f, 2.0f, 2.0f));

    DuplicateTrack d;
    d.source = 100;
    d.track = h.current().next_id;
    d.name = "A copy";
    CHECK_APPLY(h, d);
    CHECK(h.current().edit->tracks[2]->sends.size() == 1);
    // Four copies of A sum on the left: the order of the sum may round.
    CHECK(move(ctl, s, o, h.current(), 2.0f, 2.0f, 4.0f, 2.0f, 0.01f));

    CHECK_APPLY(h, RemoveTrack{d.track});
    CHECK(move(ctl, s, o, h.current(), 4.0f, 2.0f, 2.0f, 2.0f, 0.01f));
    // B goes, and its send with it: A moves up to row 0.
    CHECK_APPLY(h, RemoveTrack{102});
    CHECK(move(ctl, s, o, h.current(), 2.0f, 2.0f, 2.0f, 0.0f));
    for (int blk = 0; blk < 4; ++blk) CHECK(heard(s, o, 2.0f, 0.0f));

    CHECK(playback_test::heap_untouched());
    CHECK(s.playback_misses() == 0);
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

// #262 review N2: B over A is published, and before its mix is adopted
// the reorder is undone. The plan that puts the tracks back is built
// against the atomics as they are, in the old order, and is held at the
// residency gate while the two blocks pass and the first plan's mix is
// adopted: published with no mix of its own, it would play A on engine
// track 0 with B's hard-right pan there. Each track keeps its pan
// throughout, and the atomics end in the old order.
int test_a_plan_back_while_adopting_keeps_each_mix() {
    auto session = make_session(2);
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
    block(s, o);
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == TransportState::Rolling);
    for (int blk = 0; blk < 4; ++blk) CHECK(heard(s, o, 1.0f, 1.0f));

    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(show(ctl, h.current()));
    const uint64_t published = ctl.plans_published();
    for (int i = 0; i < 64 && ctl.plans_published() == published; ++i) {
        s.reader().run_pass();
        ctl.pump();
    }
    CHECK(ctl.plans_published() == published + 1);
    CHECK(ctl.tracks_moving());
    CHECK_APPLY(h, (ReorderTracks{{100, 102}}));
    CHECK(show(ctl, h.current()));
    // No reader pass: the plan back waits at the gate while the moving
    // plan plays its two blocks and its mix is due.
    for (int blk = 0; blk < 3; ++blk) {
        CHECK(heard(s, o, 1.0f, 1.0f));
        ctl.pump();
    }
    CHECK(ctl.plans_published() == published + 1);
    for (int i = 0; i < 64 && (ctl.tracks_moving() || ctl.plans_published() == published + 1);
         ++i) {
        s.reader().run_pass();
        ctl.pump();
        CHECK(heard(s, o, 1.0f, 1.0f));
    }
    CHECK(ctl.plans_published() == published + 2 && !ctl.tracks_moving());
    for (int blk = 0; blk < 4; ++blk) CHECK(heard(s, o, 1.0f, 1.0f));
    CHECK(s.tracks()[0]->pan() == -1.0f && s.tracks()[1]->pan() == 1.0f);
    CHECK(s.playback_misses() == 0);
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

// ADR-0042: the control thread writes each engine track's VCA factor (the
// dB of its VCAs added, 0 for a muted one) and it goes with the document
// track when a reorder moves it, as the fader does.
int test_vca_factor_follows_the_document_track() {
    auto session = make_session(3);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, "A", 0.0f, 0));
    CHECK_APPLY(h, track_op(102, "B", 0.0f, 1));
    CHECK_APPLY(h, track_op(104, "C", 0.0f, 2));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    CHECK(s.tracks()[0]->vca_gain() == 1.0f && s.tracks()[1]->vca_gain() == 1.0f);

    const Id vca = h.current().next_id;
    CHECK_APPLY(h, (SetVca{vca, "Drums", {100, 102}}));
    CHECK_APPLY(h, (SetVcaMix{VcaMix{vca, -6.0f, false}}));
    CHECK(show(ctl, h.current()));
    const float half = std::pow(10.0f, -6.0f / 20.0f);
    CHECK(std::fabs(s.tracks()[0]->vca_gain() - half) < 1e-5f);
    CHECK(std::fabs(s.tracks()[1]->vca_gain() - half) < 1e-5f);
    CHECK(s.tracks()[2]->vca_gain() == 1.0f);
    // The fader itself is not touched.
    CHECK(s.tracks()[0]->gain() == 1.0f);

    // A second VCA on A adds its dB; a muted one silences.
    const Id second = h.current().next_id;
    CHECK_APPLY(h, (SetVca{second, "Bus", {100}}));
    CHECK_APPLY(h, (SetVcaMix{VcaMix{second, -6.0f, false}}));
    CHECK(show(ctl, h.current()));
    CHECK(std::fabs(s.tracks()[0]->vca_gain() - half * half) < 1e-5f);
    CHECK(std::fabs(s.tracks()[1]->vca_gain() - half) < 1e-5f);
    CHECK_APPLY(h, (SetVcaMix{VcaMix{second, 0.0f, true}}));
    CHECK(show(ctl, h.current()));
    CHECK(s.tracks()[0]->vca_gain() == 0.0f);

    // A reorder: the factor is the track's, not the position's.
    CHECK_APPLY(h, (ReorderTracks{{104, 102, 100}}));
    CHECK(show(ctl, h.current()));
    CHECK(settle_moved(ctl, s));
    CHECK(s.tracks()[0]->vca_gain() == 1.0f);
    CHECK(std::fabs(s.tracks()[1]->vca_gain() - half) < 1e-5f);
    CHECK(s.tracks()[2]->vca_gain() == 0.0f);
    return 0;
}

// ADR-0027 §3: a touch holds its document track off its lanes, so it moves
// with that track when a reorder puts it on another engine track; the
// engine track left behind, now another track's, is not held.
int test_a_touch_follows_its_track() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, "A", 0.0f, 0));
    CHECK_APPLY(h, track_op(102, "B", 0.0f, 1));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    s.tracks()[0]->set_touched(sonora::kAutoVolume, true);

    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(show(ctl, h.current()));
    CHECK(settle_moved(ctl, s));
    CHECK(s.tracks()[0]->touched() == 0);
    CHECK(s.tracks()[1]->touched() == 1u << sonora::kAutoVolume);
    return 0;
}

// #178 review: a ParameterChange's Release while a plan moving tracks is
// pending names the track by its row in the new order, and is applied to
// the engine track holding it then, not dropped: once the plan plays, no
// track is still held.
int test_a_release_while_moving_reaches_its_track() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, "A", 0.0f, 0));
    CHECK_APPLY(h, track_op(102, "B", 0.0f, 1));
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    ctl.touch(h.current(), 0, sonora::kAutoPan, true);
    CHECK(s.tracks()[0]->touched() == 1u << sonora::kAutoPan);

    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(show(ctl, h.current()));
    CHECK(ctl.tracks_moving());
    // A is on row 1 of the document, still on engine track 0.
    ctl.touch(h.current(), 1, sonora::kAutoPan, false);
    CHECK(s.tracks()[0]->touched() == 0 && s.tracks()[1]->touched() == 0);
    // A touch while moving goes to A's engine track too, and follows it.
    ctl.touch(h.current(), 1, sonora::kAutoVolume, true);
    CHECK(s.tracks()[0]->touched() == 1u << sonora::kAutoVolume);
    CHECK(settle_moved(ctl, s));
    CHECK(s.tracks()[0]->touched() == 0);
    CHECK(s.tracks()[1]->touched() == 1u << sonora::kAutoVolume);
    // Past the end of the document: nothing to hold.
    ctl.touch(h.current(), 2, sonora::kAutoVolume, true);
    CHECK(s.tracks()[0]->touched() == 0);
    return 0;
}

std::vector<sonora::AutomationWrite> drained(Session& s) {
    std::vector<sonora::AutomationWrite> got(256);
    got.resize(s.take_automation_writes(got.data(), got.size()));
    return got;
}

// A and B, both in `mode`, rolling; then the reorder that puts B over A.
// Every block, before the plan that moves them is published and after,
// stamps one volume write of a whole block, on B's engine track only: 1
// before the swap, 0 from it on.
bool b_alone_writes_through_the_reorder(History& h, PlaybackController& ctl, Session& s) {
    Out o;
    std::string error;
    if (!h.apply(ReorderTracks{{102, 100}}, error)) {
        std::fprintf(stderr, "  reorder: %s\n", error.c_str());
        return false;
    }
    if (!show(ctl, h.current()) || !ctl.tracks_moving()) return false;
    bool swapped = false;
    int after = 0;
    for (int i = 0; i < 64 && (ctl.tracks_moving() || after < 6); ++i) {
        const uint64_t published = ctl.plans_published();
        s.reader().run_pass();
        ctl.pump();
        swapped = swapped || ctl.plans_published() != published;
        block(s, o);
        after += swapped ? 1 : 0;
        const std::vector<sonora::AutomationWrite> w = drained(s);
        const uint32_t b = swapped ? 0 : 1;
        bool ok = w.size() == 1;
        for (const sonora::AutomationWrite& x : w) {
            ok = ok && x.track == b && x.param == sonora::kAutoVolume && x.frames == kBlock;
        }
        if (!ok) {
            std::fprintf(stderr, "  block %d (%s the swap): %zu writes", i,
                         swapped ? "after" : "before", w.size());
            for (const sonora::AutomationWrite& x : w) {
                std::fprintf(stderr, " {track %u param %u frames %u}", x.track,
                             static_cast<unsigned>(x.param), x.frames);
            }
            std::fprintf(stderr, ", want one on track %u\n", b);
            return false;
        }
    }
    return swapped && !ctl.tracks_moving() && after >= 6;
}

// ADR-0027 §8 slice 2: a Latch is its document track's, as its touch is.
// B latched on engine track 1 and released, then moved over A while
// rolling: the latch moves to engine track 0 with B, and A, arriving on
// engine track 1 in Latch as well, writes nothing.
int test_a_latch_follows_its_track() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    AddTrack a = track_op(100, "A", 0.0f, 0);
    AddTrack b = track_op(102, "B", 0.0f, 1);
    a.mix.automation = AutomationMode::Latch;
    b.mix.automation = AutomationMode::Latch;
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == TransportState::Rolling);
    Out o;
    block(s, o);
    ctl.touch(h.current(), 1, sonora::kAutoVolume, true);
    block(s, o);
    ctl.touch(h.current(), 1, sonora::kAutoVolume, false);
    block(s, o);
    CHECK(s.tracks()[1]->touched() == 0);
    const std::vector<sonora::AutomationWrite> latched = drained(s);
    CHECK(latched.size() == 2);
    for (const sonora::AutomationWrite& x : latched) CHECK(x.track == 1);
    playback_test::reset_heap_counts();
    CHECK(b_alone_writes_through_the_reorder(h, ctl, s));
    CHECK(playback_test::heap_untouched());
    return 0;
}

// ADR-0027 §8 slice 2: B held in Touch and moved over A while rolling. The
// two blocks after the publish read the plan's mix, and the touched flags
// with it: they must not stamp B's hold on A, which arrives on B's old
// engine track, nor take the hold leaving that track for A's release.
int test_a_moving_plan_writes_only_the_held_track() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    AddTrack a = track_op(100, "A", 0.0f, 0);
    AddTrack b = track_op(102, "B", 0.0f, 1);
    a.mix.automation = AutomationMode::Touch;
    b.mix.automation = AutomationMode::Touch;
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);
    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == TransportState::Rolling);
    Out o;
    block(s, o);
    ctl.touch(h.current(), 1, sonora::kAutoVolume, true);
    block(s, o);
    CHECK(drained(s).size() == 1);
    playback_test::reset_heap_counts();
    CHECK(b_alone_writes_through_the_reorder(h, ctl, s));
    CHECK(playback_test::heap_untouched());
    CHECK(s.tracks()[0]->touched() == 1u << sonora::kAutoVolume && s.tracks()[1]->touched() == 0);
    return 0;
}

// ADR-0027 §8 slice 5 (#290 review B2): in Trim the pan behaves as in
// Touch, so a pan release while rolling puts the document's static pan
// back in the atomic, as Touch's does; otherwise the audio would glide to
// the ridden value while the writer records a return to the static.
int test_a_pan_release_in_trim_puts_the_static_back() {
    for (const AutomationMode mode : {AutomationMode::Touch, AutomationMode::Trim}) {
        auto session = make_session(1);
        Session& s = *session;
        History h(new_session(SessionSettings{}));
        AddTrack a = track_op(100, "A", 0.0f, 0);
        a.mix.automation = mode;
        CHECK_APPLY(h, a);
        PlaybackController ctl(s);
        CHECK(show(ctl, h.current()));
        CHECK(ctl.settle(2.0));
        ctl.play();
        CHECK(ctl.settle(2.0));
        CHECK(s.transport().state() == TransportState::Rolling);
        Out o;
        block(s, o);
        ctl.touch(h.current(), 0, sonora::kAutoPan, true);
        s.tracks()[0]->set_pan(1.0f);   // the ParameterChange moves
        block(s, o);
        ctl.touch(h.current(), 0, sonora::kAutoPan, false);
        CHECK(s.tracks()[0]->pan() == h.current().mix.find(a.track)->pan);
    }
    return 0;
}

}  // namespace

int main() {
    if (test_moves_are_heard_exactly_and_allocate_nothing() != 0) return 1;
    if (test_arm_and_input_follow_the_track() != 0) return 1;
    if (test_vca_factor_follows_the_document_track() != 0) return 1;
    if (test_atomics_wait_for_the_old_plans_last_block() != 0) return 1;
    if (test_sends_move_with_their_tracks() != 0) return 1;
    if (test_a_touch_follows_its_track() != 0) return 1;
    if (test_a_release_while_moving_reaches_its_track() != 0) return 1;
    if (test_a_latch_follows_its_track() != 0) return 1;
    if (test_a_pan_release_in_trim_puts_the_static_back() != 0) return 1;
    if (test_a_moving_plan_writes_only_the_held_track() != 0) return 1;
    if (test_a_plan_back_while_adopting_keeps_each_mix() != 0) return 1;
    std::printf("test_track_structure: ok\n");
    return 0;
}
