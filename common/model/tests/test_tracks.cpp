// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track structure (ADR-0023): RemoveTrack and DuplicateTrack, what undo
// brings back, the ids a duplicate takes, what it copies and shares, and
// both operations through the codec and the session file.
#include <memory>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

using namespace model_test;
namespace midi = sonora::midi;

namespace {

PluginRef limiter() {
    return PluginRef{PluginFormat::Lv2, "http://gareus.org/oss/lv2/dpl#mono",
                     "x42-dpl Digital Peak Limiter Mono", "0.9.2"};
}

bool refused(History& h, const Operation& op, const char* says) {
    const Snapshot before = h.current();
    std::string error;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", says);
        return false;
    }
    return mentions(error, says) && h.current() == before;
}

std::vector<Id> order(const Snapshot& s) {
    std::vector<Id> ids;
    for (const auto& t : s.edit->tracks) ids.push_back(t->id);
    return ids;
}

// Kick, Vox (two regions joined by a crossfade, a fade in, one insert with
// values, a second playlist), Bass. Vox's mix is set apart from the default.
struct Session {
    History h{new_session(settings())};
    AddTrack kick, vox, bass;
    Id source = 0, left = 0, right = 0, insert = 0, alt = 0, alt_region = 0;
};

int build(Session& s) {
    Ids ids(s.h.current());
    s.kick = add_track(ids, "Kick");
    s.vox = add_track(ids, "Vox");
    s.vox.mix.gain_db = -6.0f;
    s.vox.mix.pan = 0.25f;
    s.vox.mix.solo = true;
    s.vox.mix.solo_safe = true;
    s.vox.mix.input = {3};
    s.bass = add_track(ids, "Bass");
    CHECK_APPLY(s.h, s.kick);
    CHECK_APPLY(s.h, s.vox);
    CHECK_APPLY(s.h, s.bass);
    const AddSource src = add_source(ids, 480000, 1, s.vox.track);
    s.source = src.source.id;
    CHECK_APPLY(s.h, src);
    s.left = ids();
    Region l = region(s.left, s.source, 0, 48000, 1000);
    l.fade_in = Fade{480, FadeShape::SCurve};
    CHECK_APPLY(s.h, (PlaceRegion{s.vox.playlist, l, 0}));
    s.right = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.vox.playlist, region(s.right, s.source, 48000, 48000, 49000), 0}));
    Crossfade x;
    x.left = s.left;
    x.right = s.right;
    x.length = 240;
    CHECK_APPLY(s.h, (SetCrossfade{s.vox.playlist, x}));
    AddInsert a;
    a.track = s.vox.track;
    a.insert = s.insert = ids();
    a.slot = 2;
    a.plugin = limiter();
    a.params = {{"threshold", -3.0f}};
    CHECK_APPLY(s.h, a);
    CHECK_APPLY(s.h, (SetInsertBypass{s.insert, true}));
    s.alt = ids();
    CHECK_APPLY(s.h, (AddPlaylist{s.vox.track, s.alt, "Vox.02"}));
    s.alt_region = ids();
    CHECK_APPLY(s.h, (SetActivePlaylist{s.vox.track, s.alt}));
    CHECK_APPLY(s.h, (PlaceRegion{s.alt, region(s.alt_region, s.source, 96000, 1000), 0}));
    CHECK_APPLY(s.h, (SetActivePlaylist{s.vox.track, s.vox.playlist}));
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

int test_remove_and_undo() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    CHECK(is_undoable(RemoveTrack{s.vox.track}));
    CHECK_APPLY(s.h, RemoveTrack{s.vox.track});
    CHECK((order(s.h.current()) == std::vector<Id>{s.kick.track, s.bass.track}));
    CHECK(invariants_hold(s.h.current()));
    // Its mix and its insert's values stay, as an undone AddTrack's do, so
    // undo brings the track back with its fader and plugin where they were.
    CHECK(s.h.current().mix.find(s.vox.track) != nullptr);
    CHECK(s.h.current().mix.find_insert(s.insert) != nullptr);
    // Its source stays in the pool.
    CHECK(s.h.current().sources->find(s.source) != nullptr);

    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(s.h.current().mix == before.mix);
    CHECK(s.h.redo());
    CHECK(order(s.h.current()).size() == 2);

    // The file holds only the tracks that exist, and reads back the same.
    std::string bytes, error;
    CHECK(serialize(s.h.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(bytes.find("\"Vox\"") == std::string::npos);
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, s.h.current()));

    CHECK(refused(s.h, RemoveTrack{s.vox.track}, "does not exist"));
    CHECK(refused(s.h, RemoveTrack{0}, "does not exist"));
    return 0;
}

int test_duplicate_copies_the_active_playlist() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    DuplicateTrack d;
    d.source = s.vox.track;
    d.track = before.next_id;
    d.name = duplicate_track_name(before, s.vox.track);
    CHECK(d.name == "Vox.dup1");
    CHECK(!d.all_playlists && d.inserts);
    // Track, one playlist, its two regions, one insert.
    CHECK(duplicate_track_ids(before, d) == 5);
    CHECK(is_undoable(d));
    CHECK_APPLY(s.h, d);
    const Snapshot& after = s.h.current();
    CHECK(invariants_hold(after));
    CHECK(after.next_id == d.track + 5);

    // Directly below its source.
    CHECK((order(after) == std::vector<Id>{s.kick.track, s.vox.track, d.track, s.bass.track}));
    const TrackEdit& src = *after.edit->tracks[1];
    const TrackEdit& copy = *after.edit->tracks[2];
    CHECK(copy.name == "Vox.dup1" && copy.kind == src.kind && copy.channels == src.channels);

    // Ids rise from `track`: the playlist, its regions in order, the insert.
    CHECK(copy.playlists.size() == 1);
    const Playlist& p = *copy.playlists[0];
    CHECK(p.id == d.track + 1 && copy.active_playlist == p.id);
    CHECK(p.name == "Vox.dup1.01");
    CHECK(p.regions.size() == 2);
    CHECK(p.regions[0].id == d.track + 2 && p.regions[1].id == d.track + 3);
    // Same audio, same placement, same fades: only the ids are new.
    const Playlist& sp = playlist_of(after, 1);
    for (std::size_t i = 0; i < 2; ++i) {
        Region r = p.regions[i];
        r.id = sp.regions[i].id;
        CHECK(r == sp.regions[i]);
    }
    CHECK(p.crossfades.size() == 1);
    CHECK(p.crossfades[0].left == d.track + 2 && p.crossfades[0].right == d.track + 3);
    CHECK(p.crossfades[0].length == 240);

    CHECK(copy.inserts.size() == 1);
    CHECK(copy.inserts[0].id == d.track + 4 && copy.inserts[0].slot == 2);
    CHECK(copy.inserts[0].plugin == src.inserts[0].plugin);
    const InsertMix* values = after.mix.find_insert(d.track + 4);
    CHECK(values != nullptr && values->bypass && values->params == after.mix.find_insert(s.insert)->params);

    // The mix comes along, solo excepted (ADR-0023 §2).
    const TrackMix* m = after.mix.find(d.track);
    CHECK(m != nullptr);
    CHECK(m->gain_db == -6.0f && m->pan == 0.25f && m->solo_safe && !m->solo);
    CHECK(m->input == std::vector<std::uint16_t>{3});

    // The source is untouched, and undo takes the copy away again.
    CHECK(*after.edit->tracks[1] == *before.edit->tracks[1]);
    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(s.h.redo());
    CHECK(order(s.h.current()).size() == 4);

    // A second duplicate takes the next free name.
    CHECK(duplicate_track_name(s.h.current(), s.vox.track) == "Vox.dup2");
    return 0;
}

int test_duplicate_options() {
    Session s;
    if (build(s) != 0) return 1;
    DuplicateTrack d;
    d.source = s.vox.track;
    d.track = s.h.current().next_id;
    d.name = "Vox copy";
    d.all_playlists = true;
    d.inserts = false;
    // Track, playlist and two regions, playlist and one region.
    CHECK(duplicate_track_ids(s.h.current(), d) == 6);
    CHECK_APPLY(s.h, d);
    const TrackEdit& copy = *s.h.current().edit->tracks[2];
    CHECK(copy.inserts.empty());
    CHECK(copy.playlists.size() == 2);
    CHECK(copy.playlists[0]->id == d.track + 1 && copy.playlists[0]->regions.size() == 2);
    CHECK(copy.playlists[1]->id == d.track + 4 && copy.playlists[1]->name == "Vox copy.02");
    CHECK(copy.playlists[1]->regions.size() == 1 && copy.playlists[1]->regions[0].id == d.track + 5);
    // The copy of the source's active playlist is the copy's active one.
    CHECK(copy.active_playlist == d.track + 1);
    CHECK(s.h.current().next_id == d.track + 6);
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

int test_duplicate_midi_shares_its_sequence() {
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

    DuplicateTrack d;
    d.source = keys.track;
    d.track = h.current().next_id;
    d.name = duplicate_track_name(h.current(), keys.track);
    CHECK(duplicate_track_ids(h.current(), d) == 3);
    CHECK_APPLY(h, d);
    const TrackEdit& copy = *h.current().edit->tracks[1];
    CHECK(copy.kind == TrackKind::Midi && copy.channels == 0);
    CHECK(copy.midi_playlists.size() == 1 && copy.playlists.empty());
    const MidiRegion& r = copy.midi_playlists[0]->regions.at(0);
    CHECK(r.id == d.track + 2);
    // Shared, not copied: the duplicate costs the undo budget a few words.
    CHECK(r.data == h.current().edit->tracks[0]->midi_playlists[0]->regions[0].data);
    CHECK(invariants_hold(h.current()));
    return 0;
}

int test_duplicate_refusals() {
    Session s;
    if (build(s) != 0) return 1;
    DuplicateTrack d;
    d.source = s.vox.track;
    d.name = "Vox.dup1";
    d.track = s.h.current().next_id - 1;
    CHECK(refused(s.h, d, "already"));
    d.track = s.h.current().next_id;
    d.source = 999;
    CHECK(refused(s.h, d, "does not exist"));
    d.source = s.vox.track;
    d.name = std::string("\xff", 1);
    CHECK(refused(s.h, d, "UTF-8"));
    DuplicateTrack none;
    none.source = 999;
    CHECK(duplicate_track_ids(s.h.current(), none) == 0);
    return 0;
}

int test_codec_round_trip() {
    Session s;
    if (build(s) != 0) return 1;
    DuplicateTrack d;
    d.source = s.vox.track;
    d.track = s.h.current().next_id;
    d.name = "Vox.dup1";
    d.all_playlists = true;
    d.inserts = false;
    Compound c;
    c.label = "Duplicate Track";
    c.ops.push_back(d);
    c.ops.push_back(RemoveTrack{s.kick.track});
    for (const Operation& op : {Operation{d}, Operation{RemoveTrack{s.kick.track}}, Operation{c}}) {
        std::string bytes, error;
        CHECK(encode_operation(op, bytes, error));
        Operation back;
        CHECK(decode_operation(bytes, back, error));
        // The same operation: applied to the same state, the same result.
        Snapshot a, b;
        CHECK(apply(s.h.current(), op, a, error));
        CHECK(apply(s.h.current(), back, b, error));
        CHECK(a == b);
    }
    CHECK(std::string(operation_name(Operation{d})) == "DuplicateTrack");
    CHECK(std::string(operation_name(Operation{RemoveTrack{}})) == "RemoveTrack");
    std::string error;
    Operation out;
    CHECK(!decode_operation(R"({"op": "RemoveTrack"})", out, error));
    return 0;
}

// Sends (ADR-0022, sends and aux buses) belong to their track: a duplicate
// gets its own, with fresh ids and the same values; a delete takes them
// with the track and keeps their values, so undo brings them back as they
// were; nothing is left naming a track or send that is gone.
int test_sends_go_with_the_track() {
    Session s;
    if (build(s) != 0) return 1;
    Ids ids(s.h.current());
    const Id verb = ids();
    CHECK_APPLY(s.h, (AddBus{verb, "Verb", BusMix{}}));
    SendMix values;
    values.level_db = -6.0f;
    values.pre = true;
    values.follow_pan = false;
    values.pan = 0.5f;
    const Id send = ids();
    CHECK_APPLY(s.h, (AddSend{s.vox.track, send, 3, verb, values}));
    CHECK(invariants_hold(s.h.current()));

    DuplicateTrack d;
    d.source = s.vox.track;
    d.track = s.h.current().next_id;
    d.name = "Vox.dup1";
    // Track, playlist, two regions, insert, send.
    CHECK(duplicate_track_ids(s.h.current(), d) == 6);
    CHECK_APPLY(s.h, d);
    CHECK(invariants_hold(s.h.current()));
    const TrackEdit& copy = *s.h.current().edit->tracks[2];
    CHECK(copy.sends.size() == 1);
    CHECK(copy.sends[0].id == d.track + 5 && copy.sends[0].slot == 3 && copy.sends[0].bus == verb);
    const SendMix* m = s.h.current().mix.find_send(d.track + 5);
    CHECK(m != nullptr && m->level_db == -6.0f && m->pre && !m->follow_pan && m->pan == 0.5f);
    CHECK(s.h.current().next_id == d.track + 6);
    // The source's send is untouched.
    CHECK(s.h.current().edit->tracks[1]->sends[0].id == send);
    CHECK(s.h.undo());

    DuplicateTrack bare = d;
    bare.track = s.h.current().next_id;   // undo hands no id back
    bare.sends = false;
    CHECK(duplicate_track_ids(s.h.current(), bare) == 5);
    CHECK_APPLY(s.h, bare);
    CHECK(s.h.current().edit->tracks[2]->sends.empty());
    CHECK(s.h.undo());

    const Snapshot before = s.h.current();
    CHECK_APPLY(s.h, RemoveTrack{s.vox.track});
    CHECK(invariants_hold(s.h.current()));
    for (const auto& t : s.h.current().edit->tracks) {
        for (const Send& sd : t->sends) CHECK(sd.id != send);
    }
    // Its values stay, for undo; the bus it fed is free to go now.
    CHECK(s.h.current().mix.find_send(send) != nullptr);
    std::string error;
    Snapshot without_bus;
    CHECK(apply(s.h.current(), RemoveBus{verb}, without_bus, error));
    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    // The undone copies' insert values are pruned when the delete drops
    // their redo step (History::prune_insert_values); the rest is as it was.
    const MixState& mix = s.h.current().mix;
    CHECK(mix.tracks == before.mix.tracks && mix.sends == before.mix.sends);
    CHECK(*mix.find_insert(s.insert) == *before.mix.find_insert(s.insert));
    CHECK(s.h.current().edit->tracks[1]->sends.size() == 1);

    // The file holds the copy's send and its values, and reads them back.
    d.track = s.h.current().next_id;
    CHECK_APPLY(s.h, d);
    std::string bytes;
    CHECK(serialize(s.h.current(), WriteInfo{"test", 1}, bytes, error));
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, s.h.current()));
    return 0;
}

// What an engine that maps tracks by position asks before a member applies.
int test_order_after_and_pinned_tracks() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot& at = s.h.current();
    const Id k = s.kick.track, v = s.vox.track, b = s.bass.track;
    std::vector<Id> order;
    CHECK(track_order_after(at, MemberRef(Operation{RemoveTrack{k}}), order));
    CHECK((order == std::vector<Id>{v, b}));
    CHECK(track_order_after(at, MemberRef(Operation{DuplicateTrack{v, 99, "x", false, true}}), order));
    CHECK((order == std::vector<Id>{k, v, 99, b}));
    CHECK(track_order_after(at, MemberRef(Operation{ReorderTracks{{b, v, k}}}), order));
    CHECK((order == std::vector<Id>{b, v, k}));
    Ids ids(at);
    const AddTrack added = add_track(ids, "New");
    CHECK(track_order_after(at, MemberRef(Operation{added}), order));
    CHECK((order == std::vector<Id>{k, v, b, added.track}));
    CHECK(!track_order_after(at, MemberRef(Operation{RemoveTrack{12345}}), order));
    CHECK(!track_order_after(at, MemberRef(Operation{SetTrackName{v, "Lead"}}), order));

    // Vox (row 2) is being recorded.
    const std::vector<bool> pinned{false, true, false};
    const EditState& e = *at.edit;
    CHECK(pinned_track_refusal(e, {k, v, b, 99}, pinned, "is recording", "stop").empty());
    CHECK(pinned_track_refusal(e, {k, v, 99, b}, pinned, "is recording", "stop").empty());
    CHECK(mentions(pinned_track_refusal(e, {k, b}, pinned, "is recording", "stop recording"),
                   "\"Vox\" is recording: stop recording before deleting it"));
    CHECK(mentions(pinned_track_refusal(e, {v, b}, pinned, "is recording", "stop"),
                   "before moving it, or a track above it"));
    CHECK(mentions(pinned_track_refusal(e, {k, 99, v, b}, pinned, "is recording", "stop"),
                   "moving it"));
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
    t(test_remove_and_undo);
    t(test_duplicate_copies_the_active_playlist);
    t(test_duplicate_options);
    t(test_duplicate_midi_shares_its_sequence);
    t(test_duplicate_refusals);
    t(test_codec_round_trip);
    t(test_order_after_and_pinned_tracks);
    t(test_sends_go_with_the_track);
    if (failed != 0) {
        std::fprintf(stderr, "model_tracks: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_tracks: %d passed\n", run);
    return 0;
}
