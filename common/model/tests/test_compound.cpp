// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compound operations (ADR-0009 update 2026-10-14): several members
// validated as a whole, applied atomically, one revision, one update, at most
// one edit-undo step. A member refused halfway leaves the document as it
// was; a member sees what the members before it made.
#include <memory>
#include <string>
#include <vector>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"

using namespace model_test;

namespace {

// One mono track holding R = [100000, 500000) of a 1,000,000-frame source.
struct Base {
    History h{new_session(settings())};
    Id track = 0, playlist = 0, source = 0, r = 0;
};

int make_base(Base& b) {
    Ids ids(b.h.current());
    const AddTrack t = add_track(ids, "Vox");
    b.track = t.track;
    b.playlist = t.playlist;
    CHECK_APPLY(b.h, t);
    const AddSource s = add_source(ids, 1'000'000, 1, b.track);
    b.source = s.source.id;
    CHECK_APPLY(b.h, s);
    b.r = ids();
    CHECK_APPLY(b.h, (PlaceRegion{b.playlist, region(b.r, b.source, 100000, 400000, 200000)}));
    return 0;
}

// A take on the track, as the engine builds one: its source, then the
// RecordTake that names it, every id allocated up front.
Compound take_on(const Snapshot& s, Id track, Id playlist, Frames at, Frames length) {
    Ids ids(s);
    const AddSource src = add_source(ids, length, 1, track);
    RecordTake rec;
    rec.track = track;
    rec.playlist = playlist;
    rec.region = region(ids(), src.source.id, at, length);
    rec.split_right_id = ids();   // reserved whether or not the overwrite splits
    Compound c;
    c.ops = {src, rec};
    c.label = "Record";
    return c;
}

int test_a_member_refused_halfway_leaves_the_document_untouched() {
    Base b;
    CHECK(make_base(b) == 0);
    const Snapshot before = b.h.current();
    const std::shared_ptr<const Playlist> playlist_before =
        before.edit->tracks[0]->playlists[0];
    const Playlist playlist_value = *playlist_before;
    const std::size_t depth = b.h.undo_depth();

    // Split R, move its right part, then remove a region that is not there.
    const Id right = before.next_id;
    Compound c;
    c.ops = {SplitRegion{b.playlist, b.r, 300000, right},
             MoveRegion{b.playlist, right, 700000, 0},
             SetFade{b.playlist, b.r, FadeEnd::In, Fade{480, FadeShape::Linear}},
             RemoveRegion{b.playlist, 999999}};
    std::string err;
    CHECK(!b.h.apply(c, err));
    CHECK(mentions(err, "operation 4 of 4 (RemoveRegion)"));
    CHECK(mentions(err, "region 999999 is not on playlist"));
    // Nothing moved: the value, the revision, the id counter, the undo depth,
    // and the very playlist object (the fold edits its own copy only).
    CHECK(b.h.current() == before);
    CHECK(b.h.undo_depth() == depth);
    CHECK(b.h.current().edit->tracks[0]->playlists[0] == playlist_before);
    CHECK(*playlist_before == playlist_value);

    // The same through the Document: no update to send, nothing applied.
    Document doc(before);
    ReplicationUpdate u;
    u.revision = 12345;
    CHECK(!doc.apply(Operation{c}, u, err));
    CHECK(mentions(err, "operation 4 of 4"));
    CHECK(doc.current() == before);
    CHECK(u.revision == 12345);
    return 0;
}

int test_ids_across_members() {
    Base b;
    CHECK(make_base(b) == 0);
    const Snapshot before = b.h.current();

    // The RecordTake names the source the AddSource before it made.
    const Compound take = take_on(before, b.track, b.playlist, 200000, 50000);
    CHECK_APPLY(b.h, take);
    const Snapshot& s = b.h.current();
    const Id source = std::get<AddSource>(take.ops[0]).source.id;
    const Id region_id = std::get<RecordTake>(take.ops[1]).region.id;
    const Id split = std::get<RecordTake>(take.ops[1]).split_right_id;
    CHECK(s.sources->find(source) != nullptr);
    CHECK(find(playlist_of(s, 0), region_id) != nullptr);
    CHECK(find(playlist_of(s, 0), split) != nullptr);
    CHECK(s.next_id == split + 1);
    CHECK(invariants_hold(s));

    // A split and then the removal of its right part, in one compound.
    const Id right = s.next_id;
    Compound cut;
    cut.ops = {SplitRegion{b.playlist, split, 400000, right},
               RemoveRegion{b.playlist, right}};
    CHECK_APPLY(b.h, cut);
    CHECK(find(playlist_of(b.h.current(), 0), right) == nullptr);
    CHECK(b.h.current().next_id == right + 1);

    // A later member may not reuse an id an earlier one took.
    const Snapshot here = b.h.current();
    Ids ids(here);
    const Id m = ids();
    Compound twice;
    twice.ops = {AddMarker{Marker{m, 100, "A", 0}}, AddMarker{Marker{m, 200, "B", 0}}};
    std::string err;
    CHECK(!b.h.apply(twice, err));
    CHECK(mentions(err, "operation 2 of 2 (AddMarker)"));
    CHECK(mentions(err, "already taken"));
    CHECK(b.h.current() == here);
    return 0;
}

int test_one_revision_one_step_undo_and_redo() {
    Base b;
    CHECK(make_base(b) == 0);
    const Snapshot before = b.h.current();
    const std::size_t depth = b.h.undo_depth();

    const Compound take = take_on(before, b.track, b.playlist, 200000, 50000);
    CHECK_APPLY(b.h, take);
    const Snapshot after = b.h.current();
    CHECK(after.revision == before.revision + 1);
    CHECK(b.h.undo_depth() == depth + 1);

    // One undo removes the whole take; its source stays in the pool.
    CHECK(b.h.undo());
    CHECK(*b.h.current().edit == *before.edit);
    CHECK(b.h.current().sources->find(std::get<AddSource>(take.ops[0]).source.id) != nullptr);
    CHECK(b.h.undo_depth() == depth);
    CHECK(b.h.redo());
    CHECK(*b.h.current().edit == *after.edit);
    return 0;
}

// Mix members make no edit-undo step (the mix history is BACKLOG Track 2
// item 6); a compound of edit and mix members makes exactly one.
int test_mix_members_make_no_edit_step() {
    Base b;
    CHECK(make_base(b) == 0);
    Ids ids(b.h.current());
    const AddTrack second = add_track(ids, "Gtr");
    CHECK_APPLY(b.h, second);
    const Snapshot before = b.h.current();
    const std::size_t depth = b.h.undo_depth();

    // Alt-click solo: every track soloed in one operation, no edit step.
    Compound solo_all;
    for (const auto& t : before.edit->tracks) {
        TrackMix m = *before.mix.find(t->id);
        m.solo = true;
        solo_all.ops.push_back(SetMix{m});
    }
    CHECK(!is_undoable(Operation{solo_all}));
    CHECK_APPLY(b.h, solo_all);
    CHECK(b.h.current().revision == before.revision + 1);
    CHECK(b.h.undo_depth() == depth);
    CHECK(b.h.current().edit == before.edit);
    for (const auto& t : before.edit->tracks) CHECK(b.h.current().mix.find(t->id)->solo);

    // An edit with a mix member: one edit step; undo leaves the mix.
    TrackMix loud = *b.h.current().mix.find(b.track);
    loud.gain_db = 6.0f;
    Compound mixed;
    mixed.ops = {SetTrackName{b.track, "Lead"}, SetMix{loud}};
    CHECK(is_undoable(Operation{mixed}));
    CHECK_APPLY(b.h, mixed);
    CHECK(b.h.undo_depth() == depth + 1);
    CHECK(b.h.undo());
    CHECK(b.h.current().edit->tracks[0]->name == "Vox");
    CHECK(b.h.current().mix.find(b.track)->gain_db == 6.0f);
    return 0;
}

// The fold copies each playlist once and edits that copy; the result must
// be what applying the members one by one gives.
int test_fold_equals_members_one_by_one() {
    Base b;
    CHECK(make_base(b) == 0);
    const Snapshot before = b.h.current();
    Ids ids(before);
    const Id r1 = ids(), r2 = ids();
    Compound c;
    c.ops = {SplitRegion{b.playlist, b.r, 200000, r1},
             SplitRegion{b.playlist, r1, 300000, r2},
             MoveRegion{b.playlist, r2, 800000, 0},
             TrimRegion{b.playlist, r1, Edge::End, 250000},
             SetFade{b.playlist, b.r, FadeEnd::In, Fade{1000, FadeShape::SCurve}},
             AddMarker{Marker{ids(), 4800, "Verse", 0}},
             SetTrackName{b.track, "Lead"}};
    Snapshot folded;
    std::string err;
    CHECK(apply(before, Operation{c}, folded, err));

    Snapshot step = before;
    for (const Member& m : c.ops) {
        Snapshot next;
        CHECK(apply(step, to_operation(m), next, err));
        step = std::move(next);
    }
    CHECK(*folded.edit == *step.edit);
    CHECK(folded.next_id == step.next_id);
    CHECK(folded.revision == before.revision + 1);
    CHECK(invariants_hold(folded));
    // `before` is never modified, whatever the fold did to its own copies.
    CHECK(b.h.current() == before);
    CHECK(playlist_of(before, 0).regions.size() == 1);
    return 0;
}

int test_apply_refuses_empty_and_oversized() {
    Base b;
    CHECK(make_base(b) == 0);
    const Snapshot before = b.h.current();
    std::string err;
    CHECK(!b.h.apply(Compound{}, err));
    CHECK(mentions(err, "empty"));

    Compound big;
    TrackMix m = *before.mix.find(b.track);
    for (std::size_t i = 0; i <= kMaxCompoundMembers; ++i) big.ops.push_back(SetMix{m});
    CHECK(!b.h.apply(big, err));
    CHECK(mentions(err, std::to_string(kMaxCompoundMembers).c_str()));
    big.ops.pop_back();
    CHECK_APPLY(b.h, big);   // exactly the limit is accepted

    Compound bad_label;
    bad_label.ops = {SetTrackName{b.track, "x"}};
    bad_label.label = "\xC3";
    CHECK(!b.h.apply(bad_label, err));
    CHECK(mentions(err, "label"));
    return 0;
}

int test_codec_round_trip_and_limits() {
    Base b;
    CHECK(make_base(b) == 0);
    const Compound take = take_on(b.h.current(), b.track, b.playlist, 200000, 50000);
    std::string bytes, err;
    CHECK(encode_operation(Operation{take}, bytes, err));
    CHECK(mentions(bytes, "\"op\": \"Compound\""));
    CHECK(mentions(bytes, "\"label\": \"Record\""));
    Operation back;
    CHECK(decode_operation(bytes, back, err));
    const auto* c = std::get_if<Compound>(&back);
    CHECK(c != nullptr && c->ops.size() == 2 && c->label == "Record");
    std::string again;
    CHECK(encode_operation(back, again, err));
    CHECK(again == bytes);
    CHECK(display_name(back) == "Record");

    // No label: none on the wire, and the name is the first edit member's.
    Compound unlabelled = take;
    unlabelled.label.clear();
    CHECK(encode_operation(Operation{unlabelled}, bytes, err));
    CHECK(bytes.find("label") == std::string::npos);
    CHECK(decode_operation(bytes, back, err));
    CHECK(std::get<Compound>(back).label.empty());
    CHECK(display_name(back) == "RecordTake");
    CHECK(std::string(operation_name(back)) == "Compound");

    struct Bad {
        std::string bytes;
        const char* says;
    };
    const std::string marker = R"({"op":"RemoveMarker","marker":1})";
    std::string many = R"({"op":"Compound","ops":[)";
    for (std::size_t i = 0; i <= kMaxCompoundMembers; ++i) {
        many += (i == 0 ? "" : ",") + marker;
    }
    many += "]}";
    const std::string at_most = "at most " + std::to_string(kMaxCompoundMembers);
    const Bad bad[] = {
        {R"({"op":"Compound","ops":[]})", "empty"},
        {R"({"op":"Compound"})", "missing required key \"ops\""},
        {R"({"op":"Compound","ops":{}})", "must be an array"},
        {R"({"op":"Compound","ops":[)" + marker + R"(,{"op":"Compound","ops":[)" + marker +
             "]}]}",
         "ops[1]: a compound cannot hold a compound"},
        {R"({"op":"Compound","ops":[)" + marker + R"(,{"op":"RemoveMarker"}]})",
         "ops[1]: missing required key \"marker\""},
        {R"({"op":"Compound","ops":[)" + marker + R"(,{"op":"Explode"}]})",
         "ops[1].op: \"Explode\" is not an operation this version knows"},
        {R"({"op":"Compound","ops":[)" + marker + R"(],"label":""})", "label"},
        {R"({"op":"Compound","ops":[)" + marker + R"(],"label":7})", "label"},
        {R"({"op":"Compound","ops":[)" + marker + R"(],"extra":1})", "unknown key \"extra\""},
        {many, at_most.c_str()},
    };
    for (const Bad& x : bad) {
        Operation out = AddMarker{};
        err.clear();
        CHECK(!decode_operation(x.bytes, out, err));
        CHECK(mentions(err, x.says));
        CHECK(std::holds_alternative<AddMarker>(out));
    }
    return 0;
}

// A replica gets one update for the whole compound and stays equal.
int test_replica_sees_one_update() {
    Base b;
    CHECK(make_base(b) == 0);
    Document doc(b.h.current());
    Replica replica;
    ReplicationUpdate u;
    std::string err;
    CHECK(doc.snapshot(u, err));
    CHECK(replica.receive(u, err));

    const Compound take = take_on(doc.current(), b.track, b.playlist, 200000, 50000);
    const std::uint64_t rev = doc.current().revision;
    CHECK(doc.apply(Operation{take}, u, err));
    CHECK(u.kind == ReplicationUpdate::Kind::Op && u.revision == rev + 1);
    CHECK(replica.receive(u, err));
    CHECK(replica.current() == doc.current());
    CHECK(replica.can_undo());

    CHECK(doc.undo(u, err));
    CHECK(replica.receive(u, err));
    CHECK(replica.current() == doc.current());
    return 0;
}

// The engine's checks run per member, on the state the earlier members made:
// AddTrack then a member on that track is checked against the new track.
int test_check_hook_sees_the_intermediate_state() {
    Base b;
    CHECK(make_base(b) == 0);
    Document doc(b.h.current());
    Ids ids(doc.current());
    const AddTrack t = add_track(ids, "Keys");
    Compound c;
    c.ops = {t, SetTrackName{t.track, "Keys 2"}, SetTrackName{b.track, "Lead"}};

    std::vector<std::size_t> tracks_seen;
    const MemberCheck count = [&](const Snapshot& at, MemberRef) {
        tracks_seen.push_back(at.edit->tracks.size());
        return std::string();
    };
    ReplicationUpdate u;
    std::string err;
    CHECK(doc.apply(Operation{c}, u, err, count));
    CHECK((tracks_seen == std::vector<std::size_t>{1, 2, 2}));

    // A refusal from the hook refuses the compound like the model's own.
    const Snapshot before = doc.current();
    const MemberCheck no_renames = [](const Snapshot&, MemberRef m) {
        return m.holds<SetTrackName>() ? std::string("renames are off")
                                                       : std::string();
    };
    Compound d;
    d.ops = {AddMarker{Marker{doc.current().next_id, 0, "M", 0}},
             SetTrackName{b.track, "Again"}};
    CHECK(!doc.apply(Operation{d}, u, err, no_renames));
    CHECK(mentions(err, "operation 2 of 2 (SetTrackName): renames are off"));
    CHECK(doc.current() == before);

    // A single operation goes through the same hook, with its own words.
    CHECK(!doc.apply(Operation{SetTrackName{b.track, "Again"}}, u, err, no_renames));
    CHECK(err == "renames are off");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_a_member_refused_halfway_leaves_the_document_untouched);
    t(test_ids_across_members);
    t(test_one_revision_one_step_undo_and_redo);
    t(test_mix_members_make_no_edit_step);
    t(test_fold_equals_members_one_by_one);
    t(test_apply_refuses_empty_and_oversized);
    t(test_codec_round_trip_and_limits);
    t(test_replica_sees_one_update);
    t(test_check_hook_sees_the_intermediate_state);
    if (failed != 0) {
        std::fprintf(stderr, "model_compound: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_compound: %d passed\n", run);
    return 0;
}
