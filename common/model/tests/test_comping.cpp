// SPDX-License-Identifier: GPL-3.0-or-later
//
// Swipe comping (ADR-0031): a range of a take lane copied onto the comp as
// one compound of existing members, its silence included; the lane's own
// crossfades kept, seam crossfades fitted or left out; refusals; and the
// lane a comped region came from.
#include "model_test.h"
#include "sonora/model/comping.h"

using namespace model_test;

namespace {

const Playlist* playlist(const Snapshot& s, Id id) {
    for (const auto& t : s.edit->tracks)
        for (const auto& p : t->playlists)
            if (p->id == id) return p.get();
    return nullptr;
}

// Vox with its comp (Vox.01, active) holding take A over [0, 96000) and a
// lane (Vox.02) holding take B over the same span. Both sources are longer
// than their regions, so every join has handles either side.
struct Session {
    History h{new_session(settings())};
    AddTrack vox;
    Id lane = 0;
    Id a = 0, b = 0;     // sources
    Id main_region = 0, lane_region = 0;
};

int build(Session& s, Frames lane_position = 0, Frames lane_length = 96000) {
    Ids ids(s.h.current());
    s.vox = add_track(ids, "Vox");
    CHECK_APPLY(s.h, s.vox);
    const AddSource a = add_source(ids, 200000, 1, s.vox.track);
    const AddSource b = add_source(ids, 200000, 1, s.vox.track);
    s.a = a.source.id;
    s.b = b.source.id;
    CHECK_APPLY(s.h, a);
    CHECK_APPLY(s.h, b);
    s.main_region = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.vox.playlist, region(s.main_region, s.a, 0, 96000, 1000), 0}));
    s.lane = ids();
    CHECK_APPLY(s.h, (AddPlaylist{s.vox.track, s.lane, "Vox.02"}));
    s.lane_region = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.lane,
                                  region(s.lane_region, s.b, lane_position, lane_length,
                                         1000 + lane_position),
                                  0}));
    CHECK(s.h.current().edit->tracks[0]->active_playlist == s.vox.playlist);
    return 0;
}

CompRange swipe(const Session& s, Frames start, Frames end, Frames seam = 0) {
    CompRange c;
    c.track = s.vox.track;
    c.from = s.lane;
    c.to = s.vox.playlist;
    c.start = start;
    c.end = end;
    c.seam_crossfade = seam;
    return c;
}

bool swipe_refused(const Session& s, const CompRange& c, const char* says) {
    Ids ids(s.h.current());
    Comp out;
    std::string why;
    if (comp_range(s.h.current(), c, std::ref(ids), out, why)) {
        std::fprintf(stderr, "  built, expected \"%s\"\n", says);
        return false;
    }
    return out.members.empty() && mentions(why, says);
}

// The middle of take A replaced by the same span of take B: A is split
// around it, B's piece plays its own audio at its own place, one undo step.
int test_swipe_replaces_the_range() {
    Session s;
    if (build(s) != 0) return 1;
    const Snapshot before = s.h.current();
    Ids ids(before);
    Comp comp;
    std::string why;
    CHECK(comp_range(before, swipe(s, 24000, 48000), std::ref(ids), comp, why));
    CHECK(comp.seams == 2 && comp.seams_faded == 0);
    CHECK_APPLY(s.h, (Compound{comp.members, "Copy to Playlist"}));
    const Snapshot& after = s.h.current();
    CHECK(invariants_hold(after));
    const Playlist& m = *playlist(after, s.vox.playlist);
    CHECK(m.regions.size() == 3);
    CHECK(m.regions[0].id == s.main_region);
    CHECK(m.regions[0].position == 0 && m.regions[0].length == 24000);
    CHECK(m.regions[1].channels[0].source == s.b);
    CHECK(m.regions[1].position == 24000 && m.regions[1].length == 24000);
    CHECK(m.regions[1].source_offset == 1000 + 24000);
    CHECK(m.regions[2].channels[0].source == s.a);
    CHECK(m.regions[2].position == 48000 && m.regions[2].length == 48000);
    CHECK(m.regions[2].source_offset == 1000 + 48000);
    // Every new id was handed out, in order, and none reused.
    CHECK(after.next_id == ids.next);
    // The lane is untouched.
    CHECK(*playlist(after, s.lane) == *playlist(before, s.lane));

    const TrackEdit& vox = *after.edit->tracks[0];
    CHECK(comp_origin(vox, m.regions[1], m.id) == s.lane);
    CHECK(comp_origin(vox, m.regions[0], m.id) == 0);
    CHECK(comp_origin(vox, m.regions[2], m.id) == 0);

    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(s.h.redo());
    CHECK(playlist(s.h.current(), s.vox.playlist)->regions.size() == 3);
    return 0;
}

// A piece of the lane moved off its place no longer names the lane.
int test_origin_needs_the_same_alignment() {
    Session s;
    if (build(s) != 0) return 1;
    const TrackEdit& vox = *s.h.current().edit->tracks[0];
    Region r = playlist(s.h.current(), s.lane)->regions[0];
    r.position += 10;
    r.length -= 20;
    CHECK(comp_origin(vox, r, s.vox.playlist) == 0);
    r.source_offset += 10;
    CHECK(comp_origin(vox, r, s.vox.playlist) == s.lane);
    CHECK(comp_origin(vox, r, s.lane) == 0);
    r.channels[0].source = s.a;
    CHECK(comp_origin(vox, r, s.vox.playlist) == 0);
    return 0;
}

// The lane holds B over [30000, 40000) only: the swipe over [24000, 48000)
// leaves silence either side of it, as the lane plays.
int test_lane_silence_clears_the_comp() {
    Session s;
    if (build(s, 30000, 10000) != 0) return 1;
    Ids ids(s.h.current());
    Comp comp;
    std::string why;
    CHECK(comp_range(s.h.current(), swipe(s, 24000, 48000), std::ref(ids), comp, why));
    CHECK(comp.seams == 0);
    CHECK_APPLY(s.h, (Compound{comp.members, "Copy to Playlist"}));
    const Playlist& m = *playlist(s.h.current(), s.vox.playlist);
    CHECK(m.regions.size() == 3);
    CHECK(m.regions[0].end() == 24000);
    CHECK(m.regions[1].position == 30000 && m.regions[1].end() == 40000);
    CHECK(m.regions[2].position == 48000);
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// An empty lane over the range is Clear; nothing on either side is no edit.
int test_empty_lane() {
    Session s;
    if (build(s, 100000, 10000) != 0) return 1;
    Ids ids(s.h.current());
    Comp comp;
    std::string why;
    CHECK(comp_range(s.h.current(), swipe(s, 96000, 99000), std::ref(ids), comp, why));
    CHECK(comp.members.empty());
    CHECK(comp_range(s.h.current(), swipe(s, 0, 96000), std::ref(ids), comp, why));
    CHECK(comp.members.size() == 1);
    CHECK(holds_alternative<RemoveRegion>(comp.members[0]));
    return 0;
}

// Two lane regions joined by a crossfade the range holds whole: the copy
// keeps it, between the copies' new ids. A fade the range cut is dropped,
// one it kept stays.
int test_lane_crossfade_and_fades() {
    Session s;
    if (build(s, 0, 40000) != 0) return 1;
    Ids ids(s.h.current());
    const Id second = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.lane, region(second, s.b, 40000, 40000, 60000), 0}));
    Crossfade x;
    x.left = s.lane_region;
    x.right = second;
    x.length = 480;
    CHECK_APPLY(s.h, (SetCrossfade{s.lane, x}));
    Fade f;
    f.length = 1000;
    CHECK_APPLY(s.h, (SetFade{s.lane, s.lane_region, FadeEnd::In, f}));
    CHECK_APPLY(s.h, (SetFade{s.lane, second, FadeEnd::Out, f}));

    Ids next(s.h.current());
    Comp comp;
    std::string why;
    // Keeps the first region's fade-in, cuts the second's fade-out.
    CHECK(comp_range(s.h.current(), swipe(s, 0, 60000), std::ref(next), comp, why));
    CHECK_APPLY(s.h, (Compound{comp.members, "Copy to Playlist"}));
    const Playlist& m = *playlist(s.h.current(), s.vox.playlist);
    CHECK(m.regions.size() == 3);
    CHECK(m.regions[0].channels[0].source == s.b && m.regions[0].fade_in.length == 1000);
    CHECK(m.regions[1].channels[0].source == s.b && m.regions[1].fade_out.length == 0);
    CHECK(m.crossfades.size() == 1);
    CHECK(m.crossfades[0].left == m.regions[0].id && m.crossfades[0].right == m.regions[1].id);
    CHECK(m.crossfades[0].length == 480);
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// Quick Swipe crossfades: one at each join, centred, at the length asked
// when the handles allow.
int test_seam_crossfades() {
    Session s;
    if (build(s) != 0) return 1;
    Ids ids(s.h.current());
    Comp comp;
    std::string why;
    CHECK(comp_range(s.h.current(), swipe(s, 24000, 48000, 480), std::ref(ids), comp, why));
    CHECK(comp.seams == 2 && comp.seams_faded == 2);
    CHECK_APPLY(s.h, (Compound{comp.members, "Copy to Playlist"}));
    const Playlist& m = *playlist(s.h.current(), s.vox.playlist);
    CHECK(m.regions.size() == 3);
    CHECK(m.crossfades.size() == 2);
    CHECK(m.crossfades[0].left == m.regions[0].id && m.crossfades[0].right == m.regions[1].id);
    CHECK(m.crossfades[1].left == m.regions[1].id && m.crossfades[1].right == m.regions[2].id);
    CHECK(m.crossfades[0].length == 480 && m.crossfades[0].align == Align::Centre);
    CHECK(m.crossfades[0].shape == FadeShape::EqualPower);
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// A piece shorter than two crossfades takes at most half of itself at each
// end, so its seams never meet inside it.
int test_seam_crossfades_on_a_short_piece() {
    Session s;
    if (build(s) != 0) return 1;
    Ids ids(s.h.current());
    Comp comp;
    std::string why;
    CHECK(comp_range(s.h.current(), swipe(s, 24000, 24400, 4800), std::ref(ids), comp, why));
    CHECK(comp.seams == 2 && comp.seams_faded == 2);
    CHECK_APPLY(s.h, (Compound{comp.members, "Copy to Playlist"}));
    const Playlist& m = *playlist(s.h.current(), s.vox.playlist);
    CHECK(m.crossfades.size() == 2);
    // Each seam takes at most half of the 400-frame piece on the piece's
    // side; the neighbour's side is free to take one frame more (odd length).
    CHECK(m.regions.size() == 3 && m.regions[1].length == 400);
    Frames before = 0, after = 0;
    crossfade_extent(m.crossfades[0], before, after);
    CHECK(after <= 200);
    crossfade_extent(m.crossfades[1], before, after);
    CHECK(before <= 200);
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// A join with no audio either side of the cut takes no crossfade: the join
// stays a cut and is counted as one.
int test_seam_without_handles_stays_a_cut() {
    History h{new_session(settings())};
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddSource a = add_source(ids, 24000, 1, vox.track);
    const AddSource b = add_source(ids, 24000, 1, vox.track);
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);
    // A plays to its source's end at 24000; B starts at its source's start
    // there.
    CHECK_APPLY(h, (PlaceRegion{vox.playlist, region(ids(), a.source.id, 0, 24000, 0), 0}));
    const Id lane = ids();
    CHECK_APPLY(h, (AddPlaylist{vox.track, lane, "Vox.02"}));
    CHECK_APPLY(h, (PlaceRegion{lane, region(ids(), b.source.id, 24000, 24000, 0), 0}));
    Ids next(h.current());
    CompRange c;
    c.track = vox.track;
    c.from = lane;
    c.to = vox.playlist;
    c.start = 24000;
    c.end = 48000;
    c.seam_crossfade = 480;
    Comp comp;
    std::string why;
    CHECK(comp_range(h.current(), c, std::ref(next), comp, why));
    CHECK(comp.seams == 1 && comp.seams_faded == 0);
    CHECK_APPLY(h, (Compound{comp.members, "Copy to Playlist"}));
    CHECK(playlist(h.current(), vox.playlist)->crossfades.empty());
    CHECK(playlist(h.current(), vox.playlist)->regions.size() == 2);
    return 0;
}

int test_refusals() {
    Session s;
    if (build(s) != 0) return 1;
    CompRange c = swipe(s, 24000, 48000);
    c.from = c.to;
    CHECK(swipe_refused(s, c, "the same playlist"));
    c = swipe(s, 48000, 48000);
    CHECK(swipe_refused(s, c, "range"));
    c = swipe(s, -10, 48000);
    CHECK(swipe_refused(s, c, "range"));
    c = swipe(s, 0, 48000);
    c.track = 9999;
    CHECK(swipe_refused(s, c, "no such track"));
    c = swipe(s, 0, 48000);
    c.from = 9999;
    CHECK(swipe_refused(s, c, "not a playlist of"));

    Ids ids(s.h.current());
    AddTrack keys = add_track(ids, "Keys", 0);
    keys.kind = TrackKind::Midi;
    keys.mix.input.clear();
    CHECK_APPLY(s.h, keys);
    c = swipe(s, 0, 48000);
    c.track = keys.track;
    c.from = keys.playlist;
    c.to = keys.playlist;
    CHECK(swipe_refused(s, c, "MIDI"));
    return 0;
}

// A swipe across more pieces than one edit may hold is refused before it
// is sent, saying how many it needed.
int test_over_the_compound_limit() {
    Session s;
    if (build(s, 150000, 10) != 0) return 1;
    Ids ids(s.h.current());
    std::vector<Member> pieces;
    for (int i = 0; i < 300; ++i) {
        pieces.push_back(PlaceRegion{s.lane, region(ids(), s.b, 100000 + i * 100, 50, i * 100), 0});
    }
    for (std::size_t i = 0; i < pieces.size(); i += 200) {
        Compound part;
        for (std::size_t j = i; j < std::min(pieces.size(), i + 200); ++j) {
            part.ops.push_back(pieces[j]);
        }
        CHECK_APPLY(s.h, part);
    }
    CHECK(swipe_refused(s, swipe(s, 100000, 140000), "at most 256"));
    return 0;
}

// Consolidate (ADR-0031 §6): the rendered file replaces what the playlist
// plays over the range, as one region and one undo step; the render itself
// is the engine's.
ConsolidateRange consolidation(const Session& s, Id source, Frames start, Frames end) {
    ConsolidateRange c;
    c.track = s.vox.track;
    c.playlist = s.vox.playlist;
    c.start = start;
    c.end = end;
    c.source = source;
    c.name = "Vox_bip";
    return c;
}

int test_consolidate_replaces_the_range() {
    Session s;
    if (build(s) != 0) return 1;
    Ids ids(s.h.current());
    const AddSource rendered = add_source(ids, 48000, 1, s.vox.track);
    CHECK_APPLY(s.h, rendered);
    const Snapshot before = s.h.current();
    Ids more(before);
    Comp comp;
    std::string why;
    CHECK(consolidate_range(before, consolidation(s, rendered.source.id, 24000, 72000),
                            std::ref(more), comp, why));
    CHECK(comp.members.size() == 1);
    CHECK_APPLY(s.h, (Compound{comp.members, "Consolidate"}));
    const Snapshot& after = s.h.current();
    CHECK(invariants_hold(after));
    const Playlist& m = *playlist(after, s.vox.playlist);
    CHECK(m.regions.size() == 3);
    CHECK(m.regions[0].id == s.main_region && m.regions[0].length == 24000);
    CHECK(m.regions[1].channels[0].source == rendered.source.id);
    CHECK(m.regions[1].position == 24000 && m.regions[1].length == 48000);
    CHECK(m.regions[1].source_offset == 0 && m.regions[1].name == "Vox_bip");
    CHECK(m.regions[2].position == 72000 && m.regions[2].length == 24000);
    CHECK(s.h.undo());
    CHECK(*s.h.current().edit == *before.edit);
    return 0;
}

int test_consolidate_refusals() {
    Session s;
    if (build(s) != 0) return 1;
    Ids ids(s.h.current());
    const AddSource good = add_source(ids, 48000, 1, s.vox.track);
    const AddSource stereo = add_source(ids, 48000, 2, s.vox.track);
    const AddSource brief = add_source(ids, 100, 1, s.vox.track);
    CHECK_APPLY(s.h, good);
    CHECK_APPLY(s.h, stereo);
    CHECK_APPLY(s.h, brief);
    const Snapshot& now = s.h.current();
    const auto refused = [&](const ConsolidateRange& c, const char* says) {
        Ids n(now);
        Comp out;
        std::string why;
        if (consolidate_range(now, c, std::ref(n), out, why)) return false;
        return out.members.empty() && mentions(why, says);
    };
    CHECK(refused(consolidation(s, good.source.id, 24000, 24000), "empty"));
    CHECK(refused(consolidation(s, 9999, 0, 48000), "rendered file is not"));
    CHECK(refused(consolidation(s, stereo.source.id, 0, 48000), "channels"));
    CHECK(refused(consolidation(s, brief.source.id, 0, 48000), "shorter"));
    CHECK(refused(consolidation(s, good.source.id, 100000, 148000), "nothing to consolidate"));
    ConsolidateRange c = consolidation(s, good.source.id, 0, 48000);
    c.playlist = 12345;
    CHECK(refused(c, "not a playlist"));
    c = consolidation(s, good.source.id, 0, 48000);
    c.track = 12345;
    CHECK(refused(c, "no such track"));
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
    t(test_swipe_replaces_the_range);
    t(test_origin_needs_the_same_alignment);
    t(test_lane_silence_clears_the_comp);
    t(test_empty_lane);
    t(test_lane_crossfade_and_fades);
    t(test_seam_crossfades);
    t(test_seam_crossfades_on_a_short_piece);
    t(test_seam_without_handles_stays_a_cut);
    t(test_refusals);
    t(test_over_the_compound_limit);
    t(test_consolidate_replaces_the_range);
    t(test_consolidate_refusals);
    if (failed != 0) {
        std::fprintf(stderr, "model_comping: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_comping: %d passed\n", run);
    return 0;
}
