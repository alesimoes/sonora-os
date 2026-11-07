// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place (ADR-0051): the compound each of Freeze, Commit and Process
// makes, Unfreeze as its inverse, every refusal, one undo step each, and the
// freeze record surviving the session file, the wire and an older reader.
#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/render_in_place.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

PluginRef limiter() {
    return PluginRef{PluginFormat::Lv2, "http://gareus.org/oss/lv2/dpl#mono",
                     "x42-dpl Digital Peak Limiter Mono", "0.9.2"};
}

struct Fixture {
    History h{new_session(settings())};
    AddTrack vox;
    Id raw = 0;                  // the recorded source
    Id first = 0, second = 0;    // the inserts
    Id original = 0;             // the playlist
};

// Vox: one region at [1000, 97000) and two limiters in slots 0 and 3.
int build(Fixture& f, bool with_inserts = true) {
    Ids ids(f.h.current());
    f.vox = add_track(ids, "Vox");
    CHECK_APPLY(f.h, f.vox);
    const AddSource a = add_source(ids, 200000, 1, f.vox.track);
    f.raw = a.source.id;
    CHECK_APPLY(f.h, a);
    CHECK_APPLY(f.h, (PlaceRegion{f.vox.playlist, region(ids(), f.raw, 1000, 96000), 0}));
    f.original = f.vox.playlist;
    if (with_inserts) {
        AddInsert i;
        i.track = f.vox.track;
        i.insert = ids();
        i.slot = 3;
        i.plugin = limiter();
        f.second = i.insert;
        CHECK_APPLY(f.h, i);
        i.insert = ids();
        i.slot = 0;
        f.first = i.insert;
        CHECK_APPLY(f.h, i);
    }
    return 0;
}

const TrackEdit& vox(const Fixture& f) { return *f.h.current().edit->tracks[0]; }

bool active_of(const Fixture& f, Id insert) {
    for (const Insert& i : vox(f).inserts) {
        if (i.id == insert) return i.active;
    }
    return false;
}

const Playlist* playlist_in(const Snapshot& s, Id id) {
    for (const auto& p : s.edit->tracks[0]->playlists) {
        if (p->id == id) return p.get();
    }
    return nullptr;
}

const Playlist* playlist(const Fixture& f, Id id) { return playlist_in(f.h.current(), id); }

// What the engine does: render, add the file to the pool, then ask the model
// for the members; returns the compound.
bool make(Fixture& f, RenderKind kind, Frames sel_start, Frames sel_end, Frames file_frames,
          Compound& out, std::string& why, std::uint16_t file_channels = 1) {
    const Snapshot now = f.h.current();
    RenderRange range;
    if (!render_range_of(now, f.vox.track, kind, sel_start, sel_end, range, why)) return false;
    Ids ids(now);
    AddSource add = add_source(ids, file_frames, file_channels, f.vox.track);
    // What the engine's print is: a file the session did not record.
    add.source.kind = SourceKind::Imported;
    add.source.take = 0;
    add.source.recorded_on_track = 0;
    Compound c;
    c.ops.push_back(add);
    // The members are asked of the snapshot with the source in it.
    History probe(now);
    std::string e;
    if (!probe.apply(add, e)) {
        why = e;
        return false;
    }
    RenderPlacement p;
    p.track = f.vox.track;
    p.kind = kind;
    p.start = range.start;
    p.source = add.source.id;
    p.name = "Vox render";
    std::vector<Member> members;
    if (!render_members(probe.current(), p, std::ref(ids), members, why)) return false;
    for (Member& m : members) c.ops.push_back(std::move(m));
    c.label = kind == RenderKind::Freeze ? "Freeze Track" : "Commit Track";
    out = std::move(c);
    return true;
}

int test_freeze_plays_the_render_and_undoes_in_one_step() {
    Fixture f;
    if (build(f) != 0) return 1;
    const Snapshot before = f.h.current();
    Compound c;
    std::string why;
    CHECK(make(f, RenderKind::Freeze, 0, 0, 98000, c, why));
    CHECK_APPLY(f.h, c);
    const Snapshot& after = f.h.current();
    CHECK(invariants_hold(after));

    const Playlist* frozen = frozen_playlist(vox(f));
    CHECK(frozen != nullptr);
    CHECK(frozen->freeze->from == f.original);
    CHECK((frozen->freeze->inserts == std::vector<Id>{f.first, f.second}));   // slot order
    CHECK(frozen->regions.size() == 1);
    CHECK(frozen->regions[0].position == 1000 && frozen->regions[0].length == 98000);
    CHECK(frozen->regions[0].channels[0].source != f.raw);
    CHECK(!active_of(f, f.first) && !active_of(f, f.second));
    // The audio it was made from is untouched.
    CHECK(*playlist(f, f.original) == *playlist_in(before, f.original));
    const Id frozen_id = frozen->id;

    // One step back: the playlist, the inserts, the lot.
    CHECK(f.h.undo());
    CHECK(vox(f).active_playlist == f.original);
    CHECK(active_of(f, f.first) && active_of(f, f.second));
    CHECK(frozen_playlist(vox(f)) == nullptr);
    CHECK(playlist(f, frozen_id) == nullptr);
    return 0;
}

int test_unfreeze_returns_the_track() {
    Fixture f;
    if (build(f) != 0) return 1;
    Compound c;
    std::string why;
    CHECK(make(f, RenderKind::Freeze, 0, 0, 98000, c, why));
    CHECK_APPLY(f.h, c);
    const Id frozen_id = vox(f).active_playlist;

    std::vector<Member> back;
    CHECK(unfreeze_members(f.h.current(), f.vox.track, back, why));
    CHECK_APPLY(f.h, (Compound{back, "Unfreeze Track"}));
    CHECK(invariants_hold(f.h.current()));
    CHECK(vox(f).active_playlist == f.original);
    CHECK(active_of(f, f.first) && active_of(f, f.second));
    CHECK(playlist(f, frozen_id) == nullptr);
    // The rendered file stays in the pool: freezing again costs a render, not
    // an orphan, and undo of the unfreeze needs it.
    CHECK(f.h.current().sources->sources.size() == 2);

    // A track that is not frozen cannot be unfrozen.
    CHECK(!unfreeze_members(f.h.current(), f.vox.track, back, why) && back.empty());
    CHECK(mentions(why, "is not frozen"));

    // Undo of the unfreeze is the freeze again.
    CHECK(f.h.undo());
    CHECK(frozen_playlist(vox(f)) != nullptr && !active_of(f, f.first));
    return 0;
}

int test_commit_keeps_no_record() {
    Fixture f;
    if (build(f) != 0) return 1;
    Compound c;
    std::string why;
    CHECK(make(f, RenderKind::Commit, 0, 0, 98000, c, why));
    CHECK_APPLY(f.h, c);
    CHECK(frozen_playlist(vox(f)) == nullptr);
    CHECK(!playlist(f, vox(f).active_playlist)->freeze);
    CHECK(vox(f).active_playlist != f.original);
    CHECK(!active_of(f, f.first) && !active_of(f, f.second));
    CHECK(playlist(f, f.original) != nullptr);   // the dry take is still a playlist
    // Nothing is left to render: the inserts are inactive now.
    CHECK(!make(f, RenderKind::Commit, 0, 0, 98000, c, why));
    CHECK(mentions(why, "no active plug-in insert"));
    return 0;
}

int test_process_replaces_the_selection_on_a_copy() {
    Fixture f;
    if (build(f) != 0) return 1;
    const Snapshot before = f.h.current();
    Compound c;
    std::string why;
    CHECK(make(f, RenderKind::Process, 20000, 30000, 10000, c, why));
    CHECK_APPLY(f.h, c);
    CHECK(invariants_hold(f.h.current()));
    CHECK(vox(f).active_playlist != f.original);
    const Playlist& p = *playlist(f, vox(f).active_playlist);
    CHECK(!p.freeze);
    CHECK(p.regions.size() == 3);
    CHECK(p.regions[0].position == 1000 && p.regions[0].end() == 20000);
    CHECK(p.regions[1].position == 20000 && p.regions[1].length == 10000);
    CHECK(p.regions[1].channels[0].source != f.raw);
    CHECK(p.regions[2].position == 30000 && p.regions[2].end() == 97000);
    CHECK(p.regions[2].channels[0].source == f.raw);
    CHECK(*playlist(f, f.original) == *playlist_in(before, f.original));
    CHECK(!active_of(f, f.first));
    CHECK(f.h.undo());
    CHECK(vox(f).active_playlist == f.original && active_of(f, f.first));
    CHECK(vox(f).playlists.size() == 1);
    CHECK(!make(f, RenderKind::Process, 30000, 20000, 10000, c, why));
    return 0;
}

int test_refusals() {
    Fixture f;
    if (build(f) != 0) return 1;
    Compound c;
    std::string why;
    CHECK(!make(f, RenderKind::Process, 5000, 5000, 100, c, why));
    CHECK(mentions(why, "select a range"));
    CHECK(!make(f, RenderKind::Freeze, 0, 0, 98000, c, why, 2));
    CHECK(mentions(why, "channels"));

    // Frozen already: no second freeze, commit or process.
    CHECK(make(f, RenderKind::Freeze, 0, 0, 98000, c, why));
    CHECK_APPLY(f.h, c);
    for (const RenderKind k : {RenderKind::Freeze, RenderKind::Commit, RenderKind::Process}) {
        CHECK(!make(f, k, 0, 50000, 98000, c, why));
        CHECK(mentions(why, "frozen already"));
    }

    Fixture none;
    if (build(none, false) != 0) return 1;
    CHECK(!make(none, RenderKind::Freeze, 0, 0, 98000, c, why));
    CHECK(mentions(why, "no active plug-in insert"));

    // An empty track has nothing to render.
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack empty = add_track(ids, "Empty");
    CHECK_APPLY(h, empty);
    RenderRange range;
    CHECK(!render_range_of(h.current(), empty.track, RenderKind::Freeze, 0, 0, range, why));
    CHECK(!render_range_of(h.current(), 999, RenderKind::Freeze, 0, 0, range, why));
    CHECK(mentions(why, "no such track"));
    return 0;
}

int test_a_frozen_playlist_survives_file_wire_and_older_readers() {
    Fixture f;
    if (build(f) != 0) return 1;
    Compound c;
    std::string why;
    CHECK(make(f, RenderKind::Freeze, 0, 0, 98000, c, why));
    CHECK_APPLY(f.h, c);

    std::string bytes, error;
    CHECK(serialize(f.h.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(bytes.find("\"freeze\"") != std::string::npos);
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, f.h.current()));
    CHECK(frozen_playlist(*l.snapshot.edit->tracks[0]) != nullptr);

    // A file of version 22 has no such key; one that has it is refused, as
    // that reader would.
    std::string old = bytes;
    const std::string key = "\"sonora_session\": " + std::to_string(kFormatVersion);
    const std::size_t at = old.find(key);
    CHECK(at != std::string::npos);
    old.replace(at, key.size(), "\"sonora_session\": 22");
    Loaded l2;
    LoadError le2;
    CHECK(!deserialize(old, l2, le2));
    CHECK(mentions(le2.message, "does not define a frozen playlist"));

    // The operation on the wire, with and without the record.
    AddPlaylist add;
    add.track = 5;
    add.playlist = 9;
    add.name = "x";
    add.freeze = PlaylistFreeze{4, {7, 8}};
    std::string wire;
    CHECK(encode_operation(Operation{add}, wire, error));
    Operation back;
    CHECK(decode_operation(wire, back, error));
    const AddPlaylist* got = get_if<AddPlaylist>(&back);
    CHECK(got != nullptr && got->freeze && *got->freeze == *add.freeze);
    add.freeze.reset();
    wire.clear();
    CHECK(encode_operation(Operation{add}, wire, error));
    CHECK(wire.find("freeze") == std::string::npos);
    CHECK(decode_operation(wire, back, error));
    CHECK(!get<AddPlaylist>(back).freeze);

    // The model refuses a freeze of a playlist or an insert the track lacks.
    AddPlaylist bad;
    bad.track = f.vox.track;
    bad.playlist = f.h.current().next_id;
    bad.name = "bad";
    bad.freeze = PlaylistFreeze{12345, {}};
    std::string refusal;
    CHECK(!f.h.apply(bad, refusal) && mentions(refusal, "made from a playlist of its track"));
    bad.freeze = PlaylistFreeze{f.original, {12345}};
    CHECK(!f.h.apply(bad, refusal) && mentions(refusal, "insert that is not on its track"));
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
    t(test_freeze_plays_the_render_and_undoes_in_one_step);
    t(test_unfreeze_returns_the_track);
    t(test_commit_keeps_no_record);
    t(test_process_replaces_the_selection_on_a_copy);
    t(test_refusals);
    t(test_a_frozen_playlist_survives_file_wire_and_older_readers);
    if (failed != 0) {
        std::fprintf(stderr, "model_render_in_place: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_render_in_place: %d passed\n", run);
    return 0;
}
