// SPDX-License-Identifier: GPL-3.0-or-later
//
// Swipe comping over a target that sits in a crossfade (ADR-0031, BACKLOG
// #327 review): the range would cut one of the two regions a crossfade joins
// on the comp, so the whole swipe is refused, nothing is copied, and the
// operator's words say why. A range that clears the crossfade's extent is not
// the subject here and is left to test_comping.
#include <cstdio>
#include <functional>
#include <string>

#include "model_test.h"
#include "sonora/model/comping.h"

using namespace model_test;

namespace {

// Vox with its comp (Vox.01, active) holding take A over [0, 96000), split at
// 48000 and joined there by a 480-frame centred crossfade; and a lane (Vox.02)
// holding take B over the same span. Both sources are longer than their
// regions, so the crossfade has handles either side.
struct Session {
    History h{new_session(settings())};
    AddTrack vox;
    Id lane = 0;
    Id left = 0, right = 0;   // the comp's two halves of take A
};

int build(Session& s) {
    Ids ids(s.h.current());
    s.vox = add_track(ids, "Vox");
    CHECK_APPLY(s.h, s.vox);
    const AddSource a = add_source(ids, 200000, 1, s.vox.track);
    const AddSource b = add_source(ids, 200000, 1, s.vox.track);
    CHECK_APPLY(s.h, a);
    CHECK_APPLY(s.h, b);
    s.left = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.vox.playlist,
                                  region(s.left, a.source.id, 0, 96000, 1000), 0}));
    s.right = ids();
    CHECK_APPLY(s.h, (SplitRegion{s.vox.playlist, s.left, 48000, s.right}));
    Crossfade x;
    x.left = s.left;
    x.right = s.right;
    x.length = 480;
    CHECK_APPLY(s.h, (SetCrossfade{s.vox.playlist, x}));
    s.lane = ids();
    CHECK_APPLY(s.h, (AddPlaylist{s.vox.track, s.lane, "Vox.02"}));
    const Id lane_region = ids();
    CHECK_APPLY(s.h, (PlaceRegion{s.lane, region(lane_region, b.source.id, 0, 96000, 2000), 0}));
    return 0;
}

CompRange swipe(const Session& s, Frames start, Frames end) {
    CompRange c;
    c.track = s.vox.track;
    c.from = s.lane;
    c.to = s.vox.playlist;
    c.start = start;
    c.end = end;
    return c;
}

// The comp as the document holds it: its regions and crossfades, and the
// edit object itself, so a refusal that touched anything would show.
struct Shape {
    const void* edit = nullptr;
    std::size_t regions = 0;
    std::size_t crossfades = 0;
};

Shape shape_of(const Session& s) {
    const Snapshot& snap = s.h.current();
    const Playlist* comp = nullptr;
    for (const auto& p : snap.edit->tracks[0]->playlists)
        if (p->id == s.vox.playlist) comp = p.get();
    Shape sh;
    sh.edit = snap.edit.get();
    sh.regions = comp ? comp->regions.size() : 0;
    sh.crossfades = comp ? comp->crossfades.size() : 0;
    return sh;
}

bool same(const Shape& a, const Shape& b) {
    return a.edit == b.edit && a.regions == b.regions && a.crossfades == b.crossfades;
}

// Refused whole: no members, the operator's reason names the crossfade, and
// the document is exactly as it was before the swipe was asked for.
int refused_over_crossfade(Session& s, Frames start, Frames end, const char* label) {
    const Shape before = shape_of(s);
    Ids next(s.h.current());
    Comp out;
    std::string why;
    const bool ok = comp_range(s.h.current(), swipe(s, start, end), std::ref(next), out, why);
    if (ok) {
        std::fprintf(stderr, "  %s: built, expected a refusal\n", label);
        return 1;
    }
    if (!out.members.empty()) {
        std::fprintf(stderr, "  %s: refused but returned %zu members\n", label,
                     out.members.size());
        return 1;
    }
    if (!mentions(why, "crossfade")) {
        std::fprintf(stderr, "  %s: refusal does not name the crossfade: \"%s\"\n", label,
                     why.c_str());
        return 1;
    }
    if (!same(before, shape_of(s))) {
        std::fprintf(stderr, "  %s: the document changed on refusal\n", label);
        return 1;
    }
    return 0;
}

// A range that sits inside the crossfade's extent (240 frames either side of
// the seam at 48000) on the comp.
int test_range_inside_the_crossfade_is_refused() {
    Session s;
    if (build(s) != 0) return 1;
    return refused_over_crossfade(s, 47900, 48100, "inside the crossfade");
}

// A range that starts inside the crossfade and runs on past its far edge.
int test_range_crossing_the_crossfade_edge_is_refused() {
    Session s;
    if (build(s) != 0) return 1;
    return refused_over_crossfade(s, 48200, 60000, "crossing the crossfade edge");
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_range_inside_the_crossfade_is_refused);
    t(test_range_crossing_the_crossfade_edge_is_refused);
    if (failed != 0) {
        std::fprintf(stderr, "model_comp_crossfade_refusal: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_comp_crossfade_refusal: %d passed\n", run);
    return 0;
}
