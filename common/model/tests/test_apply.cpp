// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply(): each Phase 2 operation, the four overwrite cases, the punch-in,
// and the refusals. Every successful result is checked against the full
// invariant set, and every undoable one is undone back to the exact value.
#include "model_test.h"

using namespace model_test;

namespace {

// One mono track holding region R = [100000, 500000) of a 1,000,000-frame
// source, starting 200000 frames in: handles on both sides.
struct Base {
    History h{new_session(settings())};
    Id track = 0, playlist = 0, source = 0, r = 0;
    std::shared_ptr<const EditState> before_edit;
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
    b.before_edit = b.h.current().edit;
    CHECK(invariants_hold(b.h.current()));
    return 0;
}

// Places a fresh region of a fresh source over [a, b) and returns the
// playlist; undo must give back the base edit, pointer for pointer.
int place_and_undo(Frames a, Frames b, Id split, Playlist& out) {
    Base base;
    if (make_base(base) != 0) return 1;
    Ids ids(base.h.current());
    const AddSource s = add_source(ids, 1'000'000);
    CHECK_APPLY(base.h, s);
    const Id n = ids();
    const Id split_id = split != 0 ? ids() : 0;
    CHECK_APPLY(base.h, (PlaceRegion{base.playlist, region(n, s.source.id, a, b - a), split_id}));
    CHECK(invariants_hold(base.h.current()));
    out = playlist_of(base.h.current(), 0);
    CHECK(find(out, n) != nullptr);
    CHECK(base.h.undo());
    CHECK(base.h.current().edit == base.before_edit);
    CHECK(*base.h.current().edit == *base.before_edit);
    // Undo removed the region, never the source.
    CHECK(base.h.current().sources->find(s.source.id) != nullptr);
    return 0;
}

int test_overwrite_four_cases() {
    Playlist p;
    // 1. R entirely inside [a, b): removed.
    CHECK(place_and_undo(50000, 600000, 0, p) == 0);
    CHECK(p.regions.size() == 1);

    // 2. R straddles a: cut to end at a.
    CHECK(place_and_undo(300000, 700000, 0, p) == 0);
    CHECK(p.regions.size() == 2);
    CHECK(p.regions[0].position == 100000 && p.regions[0].length == 200000);
    CHECK(p.regions[0].source_offset == 200000);

    // 3. R straddles b: cut to start at b, the audio staying where it was.
    CHECK(place_and_undo(0, 200000, 0, p) == 0);
    CHECK(p.regions.size() == 2);
    CHECK(p.regions[1].position == 200000 && p.regions[1].length == 300000);
    CHECK(p.regions[1].source_offset == 300000);

    // 4. R spans both: split, the right part under the operation's id.
    CHECK(place_and_undo(200000, 300000, 1, p) == 0);
    CHECK(p.regions.size() == 3);
    CHECK(p.regions[0].position == 100000 && p.regions[0].end() == 200000);
    CHECK(p.regions[2].position == 300000 && p.regions[2].end() == 500000);
    CHECK(p.regions[2].source_offset == 400000);
    CHECK(p.regions[2].id != p.regions[0].id);
    return 0;
}

int test_split_needs_an_id() {
    Base b;
    CHECK(make_base(b) == 0);
    Ids ids(b.h.current());
    const AddSource s = add_source(ids, 1'000'000);
    CHECK_APPLY(b.h, s);
    std::string err;
    const Snapshot before = b.h.current();
    CHECK(!b.h.apply(PlaceRegion{b.playlist, region(ids(), s.source.id, 200000, 1000)}, err));
    CHECK(mentions(err, "split id"));
    // A refused operation changes nothing.
    CHECK(b.h.current() == before);
    return 0;
}

// The Phase 2 exit criterion, in model terms: punch over R in [A, B), with
// centred crossfades at both ends drawing on R's material past the cut and
// on the take's handle, then undo while "rolling".
int test_punch_in_and_undo() {
    Base b;
    CHECK(make_base(b) == 0);
    const Frames A = 200000, B = 300000, H = 48000, X = 240;
    Ids ids(b.h.current());
    // The take was captured from A - H to B + H.
    const AddSource take = add_source(ids, (B - A) + 2 * H, 1, b.track);
    CHECK_APPLY(b.h, take);
    const Id n = ids();
    const Id r2 = ids();
    RecordTake op;
    op.track = b.track;
    op.playlist = b.playlist;
    op.region = region(n, take.source.id, A, B - A, H);
    op.split_right_id = r2;
    op.crossfades = {Crossfade{b.r, n, X, Align::Centre, FadeShape::EqualPower},
                     Crossfade{n, r2, X, Align::Centre, FadeShape::EqualPower}};
    CHECK_APPLY(b.h, op);
    const Snapshot& s = b.h.current();
    CHECK(invariants_hold(s));
    const Playlist& p = playlist_of(s, 0);
    CHECK(p.regions.size() == 3);
    CHECK(p.regions[0].id == b.r && p.regions[0].end() == A);
    CHECK(p.regions[1].id == n && p.regions[1].position == A && p.regions[1].end() == B);
    CHECK(p.regions[2].id == r2 && p.regions[2].position == B &&
          p.regions[2].source_offset == 200000 + (B - 100000));
    CHECK(p.crossfades.size() == 2);
    CHECK(p.crossfades[0].left == b.r && p.crossfades[0].right == n);
    CHECK(p.crossfades[1].left == n && p.crossfades[1].right == r2);

    // One undo restores R whole, and only the edit moves: the take stays.
    CHECK(b.h.undo());
    CHECK(*b.h.current().edit == *b.before_edit);
    CHECK(b.h.current().sources->find(take.source.id) != nullptr);
    CHECK(b.h.redo());
    CHECK(playlist_of(b.h.current(), 0) == p);
    return 0;
}

int test_crossfade_without_handle_is_refused() {
    Base b;
    CHECK(make_base(b) == 0);
    Ids ids(b.h.current());
    const AddSource take = add_source(ids, 100000);
    CHECK_APPLY(b.h, take);
    const Id n = ids();
    RecordTake op;
    op.track = b.track;
    op.playlist = b.playlist;
    // source_offset 0: no handle before the take's start.
    op.region = region(n, take.source.id, 200000, 100000, 0);
    op.split_right_id = ids();
    op.crossfades = {Crossfade{b.r, n, 240, Align::Centre, FadeShape::EqualPower}};
    std::string err;
    const Snapshot before = b.h.current();
    CHECK(!b.h.apply(op, err));
    CHECK(mentions(err, "handle"));
    CHECK(b.h.current() == before);
    // A "post" crossfade needs no handle on the right, so it is accepted.
    op.crossfades[0].align = Align::Post;
    CHECK_APPLY(b.h, op);
    CHECK(invariants_hold(b.h.current()));
    return 0;
}

int test_record_take_on_the_wrong_track() {
    Base b;
    CHECK(make_base(b) == 0);
    Ids ids(b.h.current());
    const AddSource take = add_source(ids, 1000);
    CHECK_APPLY(b.h, take);
    RecordTake op;
    op.track = b.track + 1000;
    op.playlist = b.playlist;
    op.region = region(ids(), take.source.id, 0, 1000);
    std::string err;
    CHECK(!b.h.apply(op, err));
    CHECK(mentions(err, "is not on track"));
    return 0;
}

int test_move_trim_split_remove() {
    Base b;
    CHECK(make_base(b) == 0);
    std::string err;

    // Move: the audio moves with the region.
    CHECK_APPLY(b.h, (MoveRegion{b.playlist, b.r, 600000, 0}));
    const Region* r = find(playlist_of(b.h.current(), 0), b.r);
    CHECK(r != nullptr && r->position == 600000 && r->source_offset == 200000);

    // Trim the end past the source's last frame: refused, by name.
    CHECK(!b.h.apply(TrimRegion{b.playlist, b.r, Edge::End, 600000 + 900000}, err));
    CHECK(mentions(err, "past the end of source"));
    // Trim the start before the source's first frame: refused.
    CHECK(!b.h.apply(TrimRegion{b.playlist, b.r, Edge::Start, 300000}, err));
    CHECK(mentions(err, "frames of source before its start"));
    // A trim inside the handle keeps every frame where it was.
    CHECK_APPLY(b.h, (TrimRegion{b.playlist, b.r, Edge::Start, 500000}));
    r = find(playlist_of(b.h.current(), 0), b.r);
    CHECK(r->position == 500000 && r->source_offset == 100000 && r->length == 500000);

    // Split: left keeps the id, right takes the new one; same audio.
    const Id rid = b.h.current().next_id;
    CHECK(!b.h.apply(SplitRegion{b.playlist, b.r, 500000, rid}, err));
    CHECK(mentions(err, "not inside"));
    CHECK_APPLY(b.h, (SplitRegion{b.playlist, b.r, 700000, rid}));
    const Playlist& p = playlist_of(b.h.current(), 0);
    CHECK(p.regions.size() == 2);
    CHECK(p.regions[0].end() == 700000 && p.regions[1].position == 700000);
    CHECK(p.regions[1].source_offset == 300000);
    CHECK(invariants_hold(b.h.current()));

    // A move onto another region overwrites what it lands on.
    CHECK_APPLY(b.h, (MoveRegion{b.playlist, rid, 550000, 0}));
    CHECK(invariants_hold(b.h.current()));
    CHECK(playlist_of(b.h.current(), 0).regions.size() == 2);

    CHECK_APPLY(b.h, (RemoveRegion{b.playlist, rid}));
    CHECK(find(playlist_of(b.h.current(), 0), rid) == nullptr);
    CHECK(!b.h.apply(RemoveRegion{b.playlist, rid}, err));

    // Five undoable steps since the base; undo them all.
    for (int i = 0; i < 5; ++i) CHECK(b.h.undo());
    CHECK(*b.h.current().edit == *b.before_edit);
    return 0;
}

int test_fades_crossfades_playlists_markers_tracks() {
    Base b;
    CHECK(make_base(b) == 0);
    Ids ids(b.h.current());
    std::string err;

    CHECK_APPLY(b.h, (SetFade{b.playlist, b.r, FadeEnd::In, Fade{480, FadeShape::SCurve}}));
    CHECK(!b.h.apply(SetFade{b.playlist, b.r, FadeEnd::Out, Fade{400000, FadeShape::Linear}},
                     err));
    CHECK(mentions(err, "longer than the region"));

    // Two butting regions of one source, then a crossfade between them.
    // The second starts 500 frames into the source: room for a centred
    // 960-frame crossfade (480 before the cut), not for a "pre" one.
    const Id second = ids();
    CHECK_APPLY(b.h, (PlaceRegion{b.playlist, region(second, b.source, 500000, 100000, 500)}));
    CHECK_APPLY(b.h, (SetCrossfade{b.playlist, Crossfade{b.r, second, 960, Align::Centre,
                                                         FadeShape::EqualPower}}));
    CHECK(playlist_of(b.h.current(), 0).crossfades.size() == 1);
    // Refused when the right region has no material before its start.
    CHECK(!b.h.apply(SetCrossfade{b.playlist, Crossfade{b.r, second, 960, Align::Pre,
                                                        FadeShape::Linear}},
                     err));
    CHECK(mentions(err, "handle before its start"));
    // Moving one side apart drops the crossfade by the same rule.
    CHECK_APPLY(b.h, (MoveRegion{b.playlist, second, 700000, 0}));
    CHECK(playlist_of(b.h.current(), 0).crossfades.empty());
    CHECK(b.h.undo());
    CHECK_APPLY(b.h, (RemoveCrossfade{b.playlist, b.r, second}));
    CHECK(playlist_of(b.h.current(), 0).crossfades.empty());

    // Playlists.
    const Id alt_id = b.h.current().next_id;
    CHECK_APPLY(b.h, (AddPlaylist{b.track, alt_id, "Vox.02"}));
    CHECK_APPLY(b.h, (SetActivePlaylist{b.track, alt_id}));
    CHECK(playlist_of(b.h.current(), 0).regions.empty());
    CHECK(!b.h.apply(SetActivePlaylist{b.track, 999999}, err));

    // Markers stay sorted.
    const Id m1 = b.h.current().next_id;
    CHECK_APPLY(b.h, (AddMarker{Marker{m1, 9600000, "Chorus", 0}}));
    CHECK_APPLY(b.h, (AddMarker{Marker{m1 + 1, 4800, "Verse", 0}}));
    CHECK(b.h.current().edit->markers->markers.front().id == m1 + 1);
    CHECK_APPLY(b.h, (MoveMarker{m1 + 1, 99999999}));
    CHECK(b.h.current().edit->markers->markers.back().id == m1 + 1);
    CHECK_APPLY(b.h, (RemoveMarker{m1}));
    CHECK(b.h.current().edit->markers->markers.size() == 1);

    // Tracks: names, order, and a second track left untouched by edits.
    Ids more(b.h.current());
    const AddTrack t2 = add_track(more, "Gtr", 2);
    CHECK_APPLY(b.h, t2);
    CHECK_APPLY(b.h, (SetTrackName{b.track, "Lead Vox"}));
    CHECK_APPLY(b.h, (ReorderTracks{{t2.track, b.track}}));
    CHECK(b.h.current().edit->tracks[0]->id == t2.track);
    CHECK(!b.h.apply(ReorderTracks{{t2.track, t2.track}}, err));
    CHECK(mentions(err, "named twice"));
    CHECK(!b.h.apply(SetTrackName{b.track, "\xFF"}, err));
    CHECK(invariants_hold(b.h.current()));
    return 0;
}

int test_mix_and_settings_are_not_edits() {
    Base b;
    CHECK(make_base(b) == 0);
    const std::size_t depth = b.h.undo_depth();
    TrackMix m = *b.h.current().mix.find(b.track);
    m.gain_db = -2.5f;
    m.pan = 0.25f;
    CHECK_APPLY(b.h, SetMix{m});
    MonitorBus bus;
    bus.outputs = {{2, 3}};
    CHECK_APPLY(b.h, SetMonitorBus{bus});
    SessionSettings st = b.h.current().settings;
    st.transport.punch = {{200000, 300000}};
    CHECK_APPLY(b.h, SetSettings{st});
    // No undo step for any of them, and undo of the region keeps the fader.
    CHECK(b.h.undo_depth() == depth);
    CHECK(b.h.undo());
    CHECK(b.h.current().mix.find(b.track)->gain_db == -2.5f);
    CHECK(b.h.current().settings.transport.punch[0] == 200000);

    std::string err;
    m.pan = 2.0f;
    CHECK(!b.h.apply(SetMix{m}, err));
    CHECK(mentions(err, "pan"));
    st.sample_rate = 44100;
    CHECK(!b.h.apply(SetSettings{st}, err));
    CHECK(mentions(err, "sample rate"));
    return 0;
}

int test_ids_and_sources() {
    Base b;
    CHECK(make_base(b) == 0);
    std::string err;
    AddSource s;
    s.source.id = b.source;   // taken
    s.source.path = "Audio Files/x.wav";
    s.source.length = 10;
    CHECK(!b.h.apply(s, err));
    CHECK(mentions(err, "already taken"));
    s.source.id = b.h.current().next_id;
    s.source.path = "../outside.wav";
    CHECK(!b.h.apply(s, err));
    CHECK(mentions(err, "component"));
    s.source.path = "/abs.wav";
    CHECK(!b.h.apply(s, err));
    CHECK(mentions(err, "absolute"));
    s.source.path = "Audio Files/take_3.wav";   // the base source's path
    CHECK(!b.h.apply(s, err));
    CHECK(mentions(err, "already source"));
    // A region on a channel the source does not have.
    Region r = region(b.h.current().next_id, b.source, 0, 10);
    r.channels[0].channel = 1;
    CHECK(!b.h.apply(PlaceRegion{b.playlist, r}, err));
    CHECK(mentions(err, "has no channel 1"));
    // A mono region on a stereo track.
    Ids ids(b.h.current());
    const AddTrack st = add_track(ids, "Stereo", 2);
    CHECK_APPLY(b.h, st);
    CHECK(!b.h.apply(PlaceRegion{st.playlist, region(ids(), b.source, 0, 10)}, err));
    CHECK(mentions(err, "2-channel track"));
    return 0;
}

// apply() is pure: the input is not modified, and the result shares every
// track the operation did not touch.
int test_purity_and_sharing() {
    Base b;
    CHECK(make_base(b) == 0);
    Ids ids(b.h.current());
    CHECK_APPLY(b.h, add_track(ids, "Other"));
    const Snapshot before = b.h.current();
    const EditState deep_copy = *before.edit;
    Snapshot after;
    std::string err;
    CHECK(apply(before, MoveRegion{b.playlist, b.r, 0, 0}, after, err));
    CHECK(*before.edit == deep_copy);
    CHECK(after.revision == before.revision + 1);
    CHECK(after.edit->tracks[1] == before.edit->tracks[1]);   // shared
    CHECK(after.edit->tracks[0] != before.edit->tracks[0]);   // copied
    CHECK(after.edit->markers == before.edit->markers);
    CHECK(after.sources == before.sources);
    // The same operation on the same snapshot gives an equal snapshot:
    // what lets a replica check itself against the engine.
    Snapshot again;
    CHECK(apply(before, MoveRegion{b.playlist, b.r, 0, 0}, again, err));
    CHECK(again == after);
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
    t(test_overwrite_four_cases);
    t(test_split_needs_an_id);
    t(test_punch_in_and_undo);
    t(test_crossfade_without_handle_is_refused);
    t(test_record_take_on_the_wrong_track);
    t(test_move_trim_split_remove);
    t(test_fades_crossfades_playlists_markers_tracks);
    t(test_mix_and_settings_are_not_edits);
    t(test_ids_and_sources);
    t(test_purity_and_sharing);
    if (failed != 0) {
        std::fprintf(stderr, "model_apply: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_apply: %d passed\n", run);
    return 0;
}
