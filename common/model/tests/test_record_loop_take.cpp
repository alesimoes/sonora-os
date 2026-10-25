// SPDX-License-Identifier: GPL-3.0-or-later
//
// RecordLoopTake (ADR-0016 §3): every pass of a loop take on a new playlist
// of its own, in pass order, and the chosen pass on the active playlist
// with the punch crossfades, as one undo step; a loop take is AddSource and
// one of these per track, 2·T members whatever the pass count.
#include "model_test.h"
#include "sonora/model/op_codec.h"

using namespace model_test;

namespace {

// One mono track holding R = [100000, 500000) of a long source, with
// handles both sides; a loop A = 200000, B = 300000 inside it, and a take
// file of three whole passes and the pre-roll before them.
constexpr Frames kA = 200000, kB = 300000, kLen = kB - kA, kPre = 48000, kX = 240;

struct Base {
    History h{new_session(settings())};
    Id track = 0, playlist = 0, r = 0, take = 0;
    std::shared_ptr<const EditState> before_edit;
};

int make_base(Base& b) {
    Ids ids(b.h.current());
    const AddTrack t = add_track(ids, "Vox");
    b.track = t.track;
    b.playlist = t.playlist;
    CHECK_APPLY(b.h, t);
    const AddSource s = add_source(ids, 1'000'000, 1, b.track);
    CHECK_APPLY(b.h, s);
    b.r = ids();
    CHECK_APPLY(b.h, (PlaceRegion{b.playlist, region(b.r, s.source.id, 100000, 400000, 200000)}));
    const AddSource take = add_source(ids, kPre + 3 * kLen + 1000, 1, b.track);
    b.take = take.source.id;
    CHECK_APPLY(b.h, take);
    b.before_edit = b.h.current().edit;
    return 0;
}

// Pass k (from 0) is the file's [kPre + k·len, kPre + (k + 1)·len) at A.
RecordLoopTake loop_take(const Base& b, std::size_t passes, bool chosen) {
    Ids ids(b.h.current());
    RecordLoopTake op;
    op.track = b.track;
    op.active_playlist = b.playlist;
    for (std::size_t k = 0; k < passes; ++k) {
        LoopTakePass p;
        p.playlist = ids();
        p.name = "Vox.T3.P" + std::to_string(k + 1);
        p.region = region(ids(), b.take, kA, kLen, kPre + static_cast<Frames>(k) * kLen);
        op.passes.push_back(p);
    }
    if (chosen) {
        op.chosen = passes - 1;
        op.chosen_region = ids();
        op.split_right_id = ids();
        op.crossfades = {Crossfade{b.r, op.chosen_region, kX, Align::Centre, FadeShape::EqualPower},
                         Crossfade{op.chosen_region, op.split_right_id, kX, Align::Centre,
                                   FadeShape::EqualPower}};
    }
    return op;
}

int test_passes_on_their_playlists_and_the_last_on_the_active() {
    Base b;
    CHECK(make_base(b) == 0);
    const RecordLoopTake op = loop_take(b, 3, true);
    CHECK_APPLY(b.h, op);
    const Snapshot& s = b.h.current();
    CHECK(invariants_hold(s));
    const TrackEdit& t = *s.edit->tracks[0];
    CHECK(t.active_playlist == b.playlist);
    CHECK(t.playlists.size() == 4);
    for (std::size_t k = 0; k < 3; ++k) {
        const Playlist& p = *t.playlists[k + 1];
        CHECK(p.id == op.passes[k].playlist);
        CHECK(p.name == op.passes[k].name);
        CHECK(p.regions.size() == 1);
        CHECK(p.regions[0] == op.passes[k].region);
    }
    const Playlist& active = playlist_of(s, 0);
    CHECK(active.regions.size() == 3);
    const Region& n = active.regions[1];
    CHECK(n.id == op.chosen_region);
    CHECK(n.position == kA && n.length == kLen && n.source_offset == kPre + 2 * kLen);
    CHECK(active.regions[2].id == op.split_right_id && active.regions[2].position == kB);
    CHECK(active.crossfades.size() == 2);
    CHECK(s.next_id == op.split_right_id + 1);

    // One step: undo takes every playlist and the take away, the audio stays.
    CHECK(b.h.undo());
    CHECK(*b.h.current().edit == *b.before_edit);
    CHECK(b.h.current().sources->find(b.take) != nullptr);
    CHECK(b.h.redo());
    CHECK(playlist_of(b.h.current(), 0) == active);
    return 0;
}

// O1: with no complete pass the active playlist is left as it was.
int test_no_chosen_pass_leaves_the_active_playlist() {
    Base b;
    CHECK(make_base(b) == 0);
    const Playlist before = playlist_of(b.h.current(), 0);
    CHECK_APPLY(b.h, loop_take(b, 1, false));
    CHECK(playlist_of(b.h.current(), 0) == before);
    CHECK(b.h.current().edit->tracks[0]->playlists.size() == 2);
    CHECK(invariants_hold(b.h.current()));
    return 0;
}

int test_refusals() {
    Base b;
    CHECK(make_base(b) == 0);
    const Snapshot before = b.h.current();
    std::string err;

    RecordLoopTake none = loop_take(b, 1, false);
    none.passes.clear();
    CHECK(!b.h.apply(none, err));
    CHECK(mentions(err, "at least one pass"));

    RecordLoopTake out_of_range = loop_take(b, 2, true);
    out_of_range.chosen = 2;
    CHECK(!b.h.apply(out_of_range, err));
    CHECK(mentions(err, "pass 3"));

    RecordLoopTake stray = loop_take(b, 2, false);
    stray.crossfades = {Crossfade{b.r, 999, kX, Align::Centre, FadeShape::EqualPower}};
    CHECK(!b.h.apply(stray, err));
    CHECK(mentions(err, "crossfade"));

    RecordLoopTake wrong = loop_take(b, 1, false);
    wrong.track = b.track + 1000;
    CHECK(!b.h.apply(wrong, err));
    CHECK(mentions(err, "is not on track"));

    // A pass's region where its playlist's id should be: the ids must rise.
    RecordLoopTake reused = loop_take(b, 2, false);
    reused.passes[1].region.id = reused.passes[0].region.id;
    CHECK(!b.h.apply(reused, err));
    CHECK(mentions(err, "pass 2"));

    RecordLoopTake many = loop_take(b, kMaxLoopTakePasses + 1, false);
    CHECK(!b.h.apply(many, err));
    CHECK(mentions(err, "at most 99"));

    CHECK(b.h.current() == before);
    return 0;
}

// The wire: what is encoded decodes to the same operation, every field.
int test_codec_round_trip() {
    Base b;
    CHECK(make_base(b) == 0);
    const RecordLoopTake op = loop_take(b, 3, true);
    std::string bytes, err;
    CHECK(encode_operation(Operation{op}, bytes, err));
    Operation back;
    CHECK(decode_operation(bytes, back, err));
    const auto* got = std::get_if<RecordLoopTake>(&back);
    CHECK(got != nullptr);
    CHECK(got->track == op.track && got->active_playlist == op.active_playlist);
    CHECK(got->passes.size() == 3);
    for (std::size_t k = 0; k < 3; ++k) {
        CHECK(got->passes[k].playlist == op.passes[k].playlist);
        CHECK(got->passes[k].name == op.passes[k].name);
        CHECK(got->passes[k].region == op.passes[k].region);
    }
    CHECK(got->chosen == op.chosen && got->chosen_region == op.chosen_region);
    CHECK(got->split_right_id == op.split_right_id && got->crossfades == op.crossfades);
    CHECK(std::string(operation_name(back)) == "RecordLoopTake");
    return 0;
}

// 2·T members: an AddSource and a RecordLoopTake per track, one step.
int test_two_tracks_in_one_compound() {
    History h{new_session(settings())};
    Ids ids(h.current());
    const AddTrack a = add_track(ids, "Gtr");
    const AddTrack c = add_track(ids, "Bass");
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, c);
    const auto before = h.current().edit;
    Compound take;
    take.label = "Loop Record";
    for (const AddTrack* t : {&a, &c}) {
        const AddSource src = add_source(ids, 4 * kLen, 1, t->track);
        RecordLoopTake op;
        op.track = t->track;
        op.active_playlist = t->playlist;
        for (int k = 0; k < 3; ++k) {
            LoopTakePass p;
            p.playlist = ids();
            p.name = t->name + ".T1.P" + std::to_string(k + 1);
            p.region = region(ids(), src.source.id, kA, kLen, k * kLen);
            op.passes.push_back(p);
        }
        op.chosen = 2;
        op.chosen_region = ids();
        op.split_right_id = ids();
        take.ops.push_back(src);
        take.ops.push_back(op);
    }
    CHECK(take.ops.size() == 4);
    CHECK_APPLY(h, take);
    CHECK(h.current().edit->tracks[0]->playlists.size() == 4);
    CHECK(h.current().edit->tracks[1]->playlists.size() == 4);
    CHECK(invariants_hold(h.current()));
    CHECK(h.undo());
    CHECK(*h.current().edit == *before);
    return 0;
}

}  // namespace

int main() {
    if (test_passes_on_their_playlists_and_the_last_on_the_active()) return 1;
    if (test_no_chosen_pass_leaves_the_active_playlist()) return 1;
    if (test_refusals()) return 1;
    if (test_codec_round_trip()) return 1;
    if (test_two_tracks_in_one_compound()) return 1;
    std::printf("model_record_loop_take: ok\n");
    return 0;
}
