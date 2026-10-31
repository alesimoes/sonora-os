// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists per track (ADR-0026): Pro Tools' New, Duplicate, choose,
// rename and Delete Unused, each one undo step; the ids a duplicate takes,
// what it copies and shares; the names the interface offers; and the
// operations through the codec, the session file and the references that
// keep a sequence file, which must count an inactive playlist's regions.
#include <memory>
#include <set>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/sequence_file.h"
#include "sonora/model/session_file.h"

using namespace model_test;
namespace midi = sonora::midi;

namespace {

bool refused(History& h, const Operation& op, const char* says) {
    const Snapshot before = h.current();
    std::string error;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", says);
        return false;
    }
    return mentions(error, says) && h.current() == before;
}

const TrackEdit& track(const Snapshot& s, std::size_t i) { return *s.edit->tracks[i]; }

const Playlist* playlist(const Snapshot& s, Id id) {
    for (const auto& t : s.edit->tracks)
        for (const auto& p : t->playlists)
            if (p->id == id) return p.get();
    return nullptr;
}

// Vox: Vox.01 holding two regions joined by a crossfade; Kick: Kick.01
// empty.
struct Session {
    History h{new_session(settings())};
    AddTrack vox, kick;
    Id source = 0, left = 0, right = 0;
};

int build(Session& s) {
    Ids ids(s.h.current());
    s.vox = add_track(ids, "Vox");
    s.kick = add_track(ids, "Kick");
    CHECK_APPLY(s.h, s.vox);
    CHECK_APPLY(s.h, s.kick);
    const AddSource src = add_source(ids, 480000, 1, s.vox.track);
    s.source = src.source.id;
    CHECK_APPLY(s.h, src);
    s.left = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.vox.playlist, region(s.left, s.source, 0, 48000, 1000), 0}));
    s.right = ids();
    CHECK_APPLY(s.h,
                (PlaceRegion{s.vox.playlist, region(s.right, s.source, 48000, 48000, 49000), 0}));
    Crossfade x;
    x.left = s.left;
    x.right = s.right;
    x.length = 240;
    CHECK_APPLY(s.h, (SetCrossfade{s.vox.playlist, x}));
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// New Playlist: an empty one, made active, in one step.
int test_new_playlist_and_undo() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    CHECK(next_playlist_name(before, s.vox.track) == "Vox.02");
    const Id fresh = before.next_id;
    CHECK_APPLY(s.h, (Compound{{AddPlaylist{s.vox.track, fresh, "Vox.02"},
                                SetActivePlaylist{s.vox.track, fresh}},
                               "New Playlist"}));
    const TrackEdit& vox = track(s.h.current(), 0);
    CHECK(vox.active_playlist == fresh);
    CHECK(vox.playlists.size() == 2);
    CHECK(playlist(s.h.current(), fresh)->regions.empty());
    // The old one is as it was, shared rather than copied.
    CHECK(vox.playlists[0] == before.edit->tracks[0]->playlists[0]);
    CHECK(playlist_of(s.h.current(), 0).id == fresh);
    CHECK(invariants_hold(s.h.current()));
    CHECK(s.h.can_undo());

    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(s.h.redo());
    CHECK(track(s.h.current(), 0).active_playlist == fresh);

    // Choosing the first again is a step of its own, undone alone.
    CHECK_APPLY(s.h, (SetActivePlaylist{s.vox.track, s.vox.playlist}));
    CHECK(playlist_of(s.h.current(), 0).regions.size() == 2);
    CHECK(s.h.undo());
    CHECK(track(s.h.current(), 0).active_playlist == fresh);
    CHECK(refused(s.h, SetActivePlaylist{s.kick.track, s.vox.playlist}, "is not on track"));
    return 0;
}

int test_duplicate_playlist() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    DuplicatePlaylist d;
    d.track = s.vox.track;
    d.source = s.vox.playlist;
    d.playlist = before.next_id;
    d.name = next_playlist_name(before, s.vox.track);
    CHECK(duplicate_playlist_ids(before, d) == 3);
    CHECK(is_undoable(d));
    CHECK_APPLY(s.h, (Compound{{d, SetActivePlaylist{s.vox.track, d.playlist}},
                               "Duplicate Playlist"}));
    const Snapshot& after = s.h.current();
    CHECK(after.next_id == d.playlist + 3);
    const Playlist* copy = playlist(after, d.playlist);
    const Playlist* original = playlist(after, s.vox.playlist);
    CHECK(copy != nullptr && original != nullptr);
    CHECK(copy->name == "Vox.02");
    CHECK(track(after, 0).active_playlist == d.playlist);
    // The regions in position order, the ids after the copy's, each still
    // naming the one source; the crossfade follows its regions.
    CHECK(copy->regions.size() == 2);
    CHECK(copy->regions[0].id == d.playlist + 1 && copy->regions[1].id == d.playlist + 2);
    CHECK(copy->regions[0].channels[0].source == s.source);
    CHECK(copy->regions[0].position == original->regions[0].position);
    CHECK(copy->regions[1].source_offset == original->regions[1].source_offset);
    CHECK(copy->crossfades.size() == 1);
    CHECK(copy->crossfades[0].left == d.playlist + 1 && copy->crossfades[0].right == d.playlist + 2);
    CHECK(copy->crossfades[0].length == 240);
    CHECK(*original == *before.edit->tracks[0]->playlists[0]);
    CHECK(invariants_hold(after));

    // An edit on the copy leaves the original alone.
    CHECK_APPLY(s.h, (RemoveRegion{d.playlist, d.playlist + 2}));
    CHECK(playlist(s.h.current(), s.vox.playlist)->regions.size() == 2);

    CHECK(s.h.undo());
    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);

    // Alone, the op leaves the active playlist where it was.
    d.playlist = s.h.current().next_id;
    CHECK_APPLY(s.h, d);
    CHECK(track(s.h.current(), 0).active_playlist == s.vox.playlist);
    return 0;
}

int test_duplicate_refusals() {
    Session s;
    if (build(s) != 0) return 1;
    DuplicatePlaylist d;
    d.track = s.kick.track;
    d.source = s.vox.playlist;   // not Kick's
    d.playlist = s.h.current().next_id;
    d.name = "x";
    CHECK(duplicate_playlist_ids(s.h.current(), d) == 0);
    CHECK(refused(s.h, d, "is not on track"));
    d.track = s.vox.track;
    d.playlist = s.h.current().next_id - 1;
    CHECK(refused(s.h, d, "already taken"));
    d.playlist = s.h.current().next_id;
    d.name = std::string("\xff", 1);
    CHECK(refused(s.h, d, "UTF-8"));
    return 0;
}

int test_rename() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    CHECK_APPLY(s.h, (SetPlaylistName{s.vox.track, s.vox.playlist, "Vox lead"}));
    CHECK(playlist(s.h.current(), s.vox.playlist)->name == "Vox lead");
    // Its regions are shared with the state before: only the name changed.
    CHECK(playlist(s.h.current(), s.vox.playlist)->regions == playlist(before, s.vox.playlist)->regions);
    CHECK(playlist_name_taken(s.h.current(), "Vox lead"));
    CHECK(!playlist_name_taken(s.h.current(), "Vox lead", s.vox.playlist));
    // The lowest free number: Vox.01 is free again.
    CHECK(next_playlist_name(s.h.current(), s.vox.track) == "Vox.01");
    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(refused(s.h, SetPlaylistName{s.vox.track, s.vox.playlist, ""}, "needs a name"));
    CHECK(refused(s.h, SetPlaylistName{s.vox.track, s.vox.playlist, std::string("\xff", 1)},
                  "UTF-8"));
    CHECK(refused(s.h, SetPlaylistName{s.kick.track, s.vox.playlist, "x"}, "is not on track"));
    return 0;
}

// Delete Unused Playlists: every playlist but the active one, in one step;
// the active one cannot go, and the sources stay in the pool.
int test_delete_unused() {
    Session s;
    if (build(s) != 0) return 1;
    Ids ids(s.h.current());
    const Id empty = ids();
    CHECK_APPLY(s.h, (Compound{{AddPlaylist{s.vox.track, empty, "Vox.02"},
                                SetActivePlaylist{s.vox.track, empty}},
                               "New Playlist"}));
    const Id third = ids();
    CHECK_APPLY(s.h, (AddPlaylist{s.vox.track, third, "Vox.03"}));
    const Snapshot before = s.h.current();
    CHECK(refused(s.h, RemovePlaylist{s.vox.track, empty}, "active playlist"));
    CHECK(refused(s.h, RemovePlaylist{s.kick.track, s.vox.playlist}, "is not on track"));
    CHECK(is_undoable(RemovePlaylist{s.vox.track, third}));
    CHECK_APPLY(s.h, (Compound{{RemovePlaylist{s.vox.track, s.vox.playlist},
                                RemovePlaylist{s.vox.track, third}},
                               "Delete Unused Playlists"}));
    const TrackEdit& vox = track(s.h.current(), 0);
    CHECK(vox.playlists.size() == 1 && vox.playlists[0]->id == empty);
    CHECK(s.h.current().sources->find(s.source) != nullptr);
    CHECK(invariants_hold(s.h.current()));
    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(playlist(s.h.current(), s.vox.playlist)->regions.size() == 2);
    return 0;
}

int test_names() {
    Session s;
    if (build(s) != 0) return 1;
    // Unique in the session, as Pro Tools keeps them: "Kick.02" on Vox
    // pushes Kick's next to 03.
    Ids ids(s.h.current());
    CHECK_APPLY(s.h, (AddPlaylist{s.vox.track, ids(), "Kick.02"}));
    CHECK(next_playlist_name(s.h.current(), s.kick.track) == "Kick.03");
    CHECK(next_playlist_name(s.h.current(), 9999).empty());
    return 0;
}

int test_codec_round_trip() {
    const std::vector<Operation> ops = {DuplicatePlaylist{2, 3, 50, "Vox.02 \xC3\xA9"},
                                        SetPlaylistName{2, 3, "Lead"}, RemovePlaylist{2, 3}};
    for (const Operation& op : ops) {
        std::string bytes, error;
        CHECK(encode_operation(op, bytes, error));
        Operation back;
        CHECK(decode_operation(bytes, back, error));
        std::string again;
        CHECK(encode_operation(back, again, error));
        CHECK(again == bytes);
    }
    CHECK(std::string(operation_name(Operation{RemovePlaylist{}})) == "RemovePlaylist");
    // An unknown field is refused, as for every kind.
    Operation out;
    std::string error;
    CHECK(!decode_operation(R"({"op":"RemovePlaylist","track":1,"playlist":2,"x":1})", out, error));
    return 0;
}

// Every playlist goes in the file, its name and which is active, and comes
// back the same: no format change.
int test_session_file_keeps_every_playlist() {
    Session s;
    if (build(s) != 0) return 1;
    DuplicatePlaylist d{s.vox.track, s.vox.playlist, s.h.current().next_id, "Vox.02"};
    CHECK_APPLY(s.h, (Compound{{d, SetActivePlaylist{s.vox.track, d.playlist}}, "Duplicate"}));
    CHECK_APPLY(s.h, (SetPlaylistName{s.vox.track, s.vox.playlist, "Vox first"}));
    std::string bytes, error;
    CHECK(serialize(s.h.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(bytes.find("\"Vox first\"") != std::string::npos);
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, s.h.current()));
    CHECK(track(l.snapshot, 0).active_playlist == d.playlist);
    CHECK(track(l.snapshot, 0).playlists.size() == 2);
    return 0;
}

// A sequence that only an inactive playlist holds is still named by the
// file, so the saver writes it and cleanup keeps it (session_folder.cpp):
// deleting it would lose the take.
int test_inactive_midi_playlist_keeps_its_sequence() {
    History h(new_session(settings()));
    Ids ids(h.current());
    AddTrack keys;
    keys.track = ids();
    keys.playlist = ids();
    keys.name = "Keys";
    keys.playlist_name = "Keys.01";
    keys.kind = TrackKind::Midi;
    keys.channels = 0;
    CHECK_APPLY(h, keys);
    MidiLists lists;
    midi::Note n;
    n.start = 0;
    n.length = 480;
    n.pitch = 60;
    n.velocity = 100;
    lists.notes = {n};
    PlaceMidiRegion place;
    place.playlist = keys.playlist;
    place.region.id = ids();
    place.region.name = "m";
    place.region.length = 960;
    place.region.data = std::make_shared<const MidiSequence>(std::move(lists));
    CHECK_APPLY(h, place);

    // A duplicate shares the sequence; a new empty playlist becomes active.
    DuplicatePlaylist d{keys.track, keys.playlist, h.current().next_id, "Keys.02"};
    CHECK(duplicate_playlist_ids(h.current(), d) == 2);
    CHECK_APPLY(h, d);
    const auto& dup = track(h.current(), 0).midi_playlists.back();
    CHECK(dup->id == d.playlist && dup->regions.at(0).id == d.playlist + 1);
    CHECK(dup->regions[0].data == place.region.data);
    const Id empty = h.current().next_id;
    CHECK_APPLY(h, (Compound{{AddPlaylist{keys.track, empty, "Keys.03"},
                              SetActivePlaylist{keys.track, empty}},
                             "New Playlist"}));
    CHECK(invariants_hold(h.current()));

    SequenceDigests digests;
    const std::vector<NamedSequence> named = named_sequences(h.current(), digests);
    CHECK(named.size() == 1);
    std::string bytes, error;
    CHECK(serialize(h.current(), WriteInfo{"test", 1}, bytes, error, &digests));
    std::set<std::string> refs;
    CHECK(sequence_references(bytes, refs, error));
    CHECK(refs.count(named[0].ref.sha256) == 1);
    return 0;
}

// What every engine asks before a member, an undo or a redo during a take.
int test_active_playlist_changes() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    const Id alt = before.next_id;
    const SetActivePlaylist to_alt{s.vox.track, alt};
    CHECK_APPLY(s.h, (AddPlaylist{s.vox.track, alt, "Vox.02"}));
    const Snapshot at = s.h.current();
    CHECK((active_playlist_changes(at, MemberRef(Operation{to_alt})) == std::vector<Id>{s.vox.track}));
    // Choosing the one already active changes nothing.
    const Operation same = SetActivePlaylist{s.vox.track, s.vox.playlist};
    CHECK(active_playlist_changes(at, MemberRef(same)).empty());
    const Operation other = AddMarker{};
    CHECK(active_playlist_changes(at, MemberRef(other)).empty());
    CHECK_APPLY(s.h, to_alt);
    CHECK((active_playlist_changes(*at.edit, *s.h.current().edit) ==
           std::vector<Id>{s.vox.track}));
    CHECK(active_playlist_changes(*before.edit, *at.edit).empty());

    const std::vector<Id> vox = {s.vox.track};
    CHECK(pinned_playlist_refusal(*at.edit, vox, {false, true}).empty());   // Kick records
    CHECK(mentions(pinned_playlist_refusal(*at.edit, vox, {true, false}),
                   "\"Vox\" is recording: stop recording before choosing another playlist"));
    CHECK(mentions(take_closing_playlist_refusal(4), "take 4 is still closing"));
    CHECK(mentions(take_starting_playlist_refusal(), "about to start"));
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
    t(test_new_playlist_and_undo);
    t(test_duplicate_playlist);
    t(test_duplicate_refusals);
    t(test_rename);
    t(test_delete_unused);
    t(test_names);
    t(test_codec_round_trip);
    t(test_session_file_keeps_every_playlist);
    t(test_inactive_midi_playlist_keeps_its_sequence);
    t(test_active_playlist_changes);
    if (failed != 0) {
        std::fprintf(stderr, "model_playlists: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_playlists: %d passed\n", run);
    return 0;
}
