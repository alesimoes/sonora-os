// SPDX-License-Identifier: GPL-3.0-or-later
//
// Range-local validation and editing (BACKLOG Track 2 item 8, ADR-0009 update
// 2026-10-14 item 5). Each member of a compound re-checks only the regions it
// touched, one either side, and their crossfades. This binary links the
// audited model library, which runs every playlist edit a second time through
// the whole-playlist operators and playlist_problem(), and aborts unless both
// reach the same verdict, in the same words, on the same playlist. The
// property test below drives that audit with random member sequences on
// dense, crossfaded playlists, where the neighbourhoods matter.
#include <cstdint>
#include <random>

#include "../src/checks.h"
#include "../src/local_edit.h"
#include "model_test.h"

using namespace model_test;

namespace {

class Gen {
public:
    explicit Gen(std::uint64_t seed) : rng_(seed) {}
    // rng() % n, not a std distribution: those are not specified bit for
    // bit, and the sequence must be the same everywhere.
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : rng_() % n; }
    Frames frames(Frames lo, Frames hi) {
        return lo + static_cast<Frames>(below(static_cast<std::uint64_t>(hi - lo + 1)));
    }
    bool chance(int percent) { return below(100) < static_cast<std::uint64_t>(percent); }

private:
    std::mt19937_64 rng_;
};

constexpr Frames kSourceLength = 50'000'000;
constexpr Frames kHandle = 1'000'000;   // source before every region's start

struct Track {
    Id track = 0, playlist = 0, source = 0;
    std::uint16_t channels = 1;
};

struct Session {
    Snapshot s;
    std::vector<Track> tracks;
};

bool step(Snapshot& s, const Operation& op) {
    Snapshot next;
    std::string err;
    if (!apply(s, op, next, err)) {
        std::fprintf(stderr, "  setup refused: %s\n", err.c_str());
        return false;
    }
    s = std::move(next);
    return true;
}

Crossfade random_xfade(Id left, Id right, Gen& g) {
    static const Align aligns[] = {Align::Centre, Align::Pre, Align::Post};
    return Crossfade{left, right, g.frames(1, 3000), aligns[g.below(3)], FadeShape::EqualPower};
}

Crossfade centred(Id left, Id right, Frames length) {
    return Crossfade{left, right, length, Align::Centre, FadeShape::EqualPower};
}

// A mono and a stereo track, each a run of regions that mostly butt, with
// fades and crossfades: the shape a comped vocal leaves.
int build(Gen& g, Session& out) {
    Snapshot s = new_session(settings());
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        Ids ids(s);
        Track t;
        const AddTrack at = add_track(ids, "T" + std::to_string(channels), channels);
        t.track = at.track;
        t.playlist = at.playlist;
        t.channels = channels;
        CHECK(step(s, at));
        const AddSource src = add_source(ids, kSourceLength, channels, t.track);
        t.source = src.source.id;
        CHECK(step(s, src));
        Frames cursor = 0;
        for (int i = 0; i < 60; ++i) {
            Region r = region(s.next_id, t.source, cursor, g.frames(2000, 20000),
                              cursor + kHandle, channels);
            if (g.chance(30)) r.fade_in = Fade{g.frames(0, r.length / 3), FadeShape::Linear};
            if (g.chance(30)) r.fade_out = Fade{g.frames(0, r.length / 3), FadeShape::SCurve};
            CHECK(step(s, PlaceRegion{t.playlist, r, 0}));
            cursor = r.end() + (g.chance(65) ? 0 : g.frames(1, 5000));
        }
        const std::vector<Region> rs = playlist_of(s, s.edit->tracks.size() - 1).regions;
        for (std::size_t i = 1; i < rs.size(); ++i) {
            if (rs[i - 1].end() != rs[i].position || !g.chance(70)) continue;
            Snapshot next;
            std::string err;
            // Some are refused (a fade in the way); those are left out.
            if (apply(s, SetCrossfade{t.playlist, random_xfade(rs[i - 1].id, rs[i].id, g)}, next,
                      err)) {
                s = std::move(next);
            }
        }
        out.tracks.push_back(t);
    }
    CHECK(invariants_hold(s));
    out.s = std::move(s);
    return 0;
}

const Playlist& playlist_by_id(const Snapshot& s, Id id) {
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->playlists) {
            if (p->id == id) return *p;
        }
    }
    return *s.edit->tracks.front()->playlists.front();
}

// One member, drawn from the state the members before it made, so most of
// them name real regions and succeed; the rest exercise refusals.
Operation next_member(const Snapshot& s, const std::vector<Track>& tracks, Gen& g) {
    const Track& t = tracks[g.below(tracks.size())];
    const Playlist& p = playlist_by_id(s, t.playlist);
    const Id next = s.next_id;
    const std::uint64_t kind = g.below(11);
    if (p.regions.empty() || kind == 0) {
        const Frames at = g.frames(0, 800'000);
        Region r = region(next, t.source, at, g.frames(500, 30000), at + kHandle, t.channels);
        if (g.chance(20)) r.fade_out = Fade{g.frames(0, r.length), FadeShape::Linear};
        return PlaceRegion{t.playlist, r, next + 1};
    }
    const std::size_t i = g.below(p.regions.size());
    const Region& r = p.regions[i];
    switch (kind) {
        case 1:
            return SplitRegion{t.playlist, r.id, r.position + g.frames(0, r.length), next};
        case 2:
            return RemoveRegion{t.playlist, g.chance(97) ? r.id : next + 7};
        case 3:
            // A nudge mostly, sometimes far: both ends of a move are touched.
            return MoveRegion{t.playlist, r.id,
                              g.chance(70) ? std::max<Frames>(0, r.position + g.frames(-8000, 8000))
                                           : g.frames(0, 800'000),
                              next};
        case 4:
            // Often a crossfade's right region, back to just inside its left
            // one, which then may be too short for the crossfades at both
            // its ends.
            if (!p.crossfades.empty() && g.chance(50)) {
                const Crossfade& x = p.crossfades[g.below(p.crossfades.size())];
                for (const Region& left : p.regions) {
                    if (left.id == x.left) {
                        return TrimRegion{t.playlist, x.right, Edge::Start,
                                          left.position + g.frames(1, 5000)};
                    }
                }
            }
            return TrimRegion{t.playlist, r.id, g.chance(50) ? Edge::Start : Edge::End,
                              r.position + g.frames(-10000, r.length + 10000)};
        case 5: {
            // A punch: its crossfades join whatever straddles its edges.
            RecordTake op;
            op.track = t.track;
            op.playlist = t.playlist;
            const Frames at = r.position + g.frames(1, r.length);
            op.region = region(next, t.source, at, g.frames(500, 20000), at + kHandle, t.channels);
            op.split_right_id = next + 1;
            for (const Region& o : p.regions) {
                if (o.position < at && o.end() > at && g.chance(80)) {
                    op.crossfades.push_back(random_xfade(o.id, next, g));
                }
                if (o.position < op.region.end() && o.end() > op.region.end() && g.chance(60)) {
                    const Id right = o.position < at ? next + 1 : o.id;
                    op.crossfades.push_back(random_xfade(next, right, g));
                }
            }
            return op;
        }
        case 6:
        case 7:
            return SetFade{t.playlist, r.id, g.chance(50) ? FadeEnd::In : FadeEnd::Out,
                           Fade{g.frames(0, r.length), FadeShape::Linear}};
        case 8:
        case 10: {
            std::vector<std::size_t> butting;
            for (std::size_t j = 1; j < p.regions.size(); ++j) {
                if (p.regions[j - 1].end() == p.regions[j].position) butting.push_back(j);
            }
            if (butting.empty()) break;
            const std::size_t b = butting[g.below(butting.size())];
            return SetCrossfade{t.playlist,
                                random_xfade(p.regions[b - 1].id, p.regions[b].id, g)};
        }
        case 9:
            if (p.crossfades.empty()) break;
            {
                const Crossfade& x = p.crossfades[g.below(p.crossfades.size())];
                return RemoveCrossfade{t.playlist, x.left, x.right};
            }
        default:
            break;
    }
    return SetFade{t.playlist, r.id, FadeEnd::In, Fade{0, FadeShape::Linear}};
}

std::size_t crossfades_in(const Snapshot& s) {
    std::size_t n = 0;
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->playlists) n += p->crossfades.size();
    }
    return n;
}

// Z | A | B, butting, crossfaded at both of A's ends, with a long fade-out on
// A that only the crossfade into B makes legal. B moves away: that crossfade
// goes, and A's fade-out, heard again, is shortened to meet the crossfade
// from Z instead of being refused.
int test_a_move_fits_the_fade_it_uncovers() {
    Snapshot s = new_session(settings());
    Ids ids(s);
    const AddTrack t = add_track(ids, "Vox");
    CHECK(step(s, t));
    const AddSource src = add_source(ids, kSourceLength, 1, t.track);
    CHECK(step(s, src));
    const Id z = ids(), a = ids(), b = ids();
    const Id source = src.source.id;
    CHECK(step(s, PlaceRegion{t.playlist, region(z, source, 0, 10000, kHandle), 0}));
    CHECK(step(s, PlaceRegion{t.playlist, region(a, source, 10000, 10000, 10000 + kHandle), 0}));
    CHECK(step(s, PlaceRegion{t.playlist, region(b, source, 20000, 10000, 20000 + kHandle), 0}));
    CHECK(step(s, SetCrossfade{t.playlist, centred(z, a, 2000)}));
    CHECK(step(s, SetCrossfade{t.playlist, centred(a, b, 2000)}));
    CHECK(step(s, SetFade{t.playlist, a, FadeEnd::Out, Fade{9500, FadeShape::Linear}}));

    Compound c;
    c.ops = {MoveRegion{t.playlist, b, 100000, s.next_id}};
    CHECK(step(s, c));
    const Playlist& p = playlist_of(s, 0);
    CHECK(p.crossfades.size() == 1);
    CHECK(p.crossfades[0].left == z && p.crossfades[0].right == a);
    CHECK(find(p, a)->fade_out.length == 10000 - 1000);
    CHECK(invariants_hold(s));
    return 0;
}

// Z | A | B, crossfaded at both of A's ends, 2,000 frames into A from each.
// B's start is trimmed back to 2,500 frames into A: A still butts B, but
// both crossfades no longer fit in A. The one into B goes, by the rule that
// reads what the crossfade from Z left on A, outside the trimmed range.
int test_a_trim_drops_the_crossfade_that_no_longer_fits() {
    Snapshot s = new_session(settings());
    Ids ids(s);
    const AddTrack t = add_track(ids, "Bass");
    CHECK(step(s, t));
    const AddSource src = add_source(ids, kSourceLength, 1, t.track);
    CHECK(step(s, src));
    const Id z = ids(), a = ids(), b = ids();
    const Id source = src.source.id;
    CHECK(step(s, PlaceRegion{t.playlist, region(z, source, 0, 10000, kHandle), 0}));
    CHECK(step(s, PlaceRegion{t.playlist, region(a, source, 10000, 10000, 10000 + kHandle), 0}));
    CHECK(step(s, PlaceRegion{t.playlist, region(b, source, 20000, 10000, 20000 + kHandle), 0}));
    CHECK(step(s, SetCrossfade{t.playlist, centred(z, a, 4000)}));
    CHECK(step(s, SetCrossfade{t.playlist, centred(a, b, 4000)}));

    Compound c;
    c.ops = {TrimRegion{t.playlist, b, Edge::Start, 12500}};
    CHECK(step(s, c));
    const Playlist& p = playlist_of(s, 0);
    CHECK(find(p, a)->length == 2500);
    CHECK(find(p, b)->position == 12500);
    CHECK(p.crossfades.size() == 1);
    CHECK(p.crossfades[0].left == z && p.crossfades[0].right == a);
    CHECK(invariants_hold(s));
    return 0;
}

// Ten butting regions crossfaded end to end. A cut out of the middle of one
// (split, split, remove the middle) keeps the crossfades on either side, the
// right one now on the region left of the cut's end, and the list stays in
// region order.
int test_a_cut_keeps_the_crossfades_beside_it() {
    Snapshot s = new_session(settings());
    Ids ids(s);
    const AddTrack t = add_track(ids, "Gtr");
    CHECK(step(s, t));
    const AddSource src = add_source(ids, kSourceLength, 1, t.track);
    CHECK(step(s, src));
    std::vector<Id> r;
    for (int i = 0; i < 10; ++i) {
        r.push_back(ids());
        const Frames at = 10000 * i;
        const Region placed = region(r.back(), src.source.id, at, 10000, at + kHandle);
        CHECK(step(s, PlaceRegion{t.playlist, placed, 0}));
    }
    for (int i = 1; i < 10; ++i) {
        CHECK(step(s, SetCrossfade{t.playlist, centred(r[i - 1], r[i], 1000)}));
    }
    Ids more(s);
    const Id middle = more(), right = more();
    Compound cut;
    cut.label = "Cut";
    cut.ops = {SplitRegion{t.playlist, r[5], 53000, middle},
               SplitRegion{t.playlist, middle, 57000, right},
               RemoveRegion{t.playlist, middle}};
    CHECK(step(s, cut));
    const Playlist& p = playlist_of(s, 0);
    CHECK(p.regions.size() == 11);
    CHECK(p.crossfades.size() == 9);
    for (std::size_t k = 0; k < p.crossfades.size(); ++k) {
        const Region* l = find(p, p.crossfades[k].left);
        CHECK(l != nullptr);
        if (k > 0) CHECK(find(p, p.crossfades[k - 1].left)->position < l->position);
    }
    CHECK(p.crossfades[5].left == right && p.crossfades[5].right == r[6]);
    CHECK(invariants_hold(s));
    return 0;
}

// A watched crossfade listed before an unwatched one whose left region comes
// first. No operator builds that list today, so it is made by hand: the
// local check must still refuse it, at the same index and in the same words
// as playlist_problem(), and not only in audited builds.
int test_a_watched_crossfade_before_an_earlier_one_is_unsorted() {
    Snapshot s = new_session(settings());
    Ids ids(s);
    const AddTrack t = add_track(ids, "Keys");
    CHECK(step(s, t));
    const AddSource src = add_source(ids, kSourceLength, 1, t.track);
    CHECK(step(s, src));
    std::vector<Id> r;
    for (int i = 0; i < 6; ++i) {
        r.push_back(ids());
        const Frames at = 10000 * i;
        CHECK(step(s, PlaceRegion{t.playlist, region(r.back(), src.source.id, at, 10000,
                                                     at + kHandle),
                                  0}));
    }
    CHECK(step(s, SetCrossfade{t.playlist, centred(r[0], r[1], 1000)}));
    CHECK(step(s, SetCrossfade{t.playlist, centred(r[4], r[5], 1000)}));

    Playlist p = playlist_of(s, 0);
    CHECK(p.crossfades.size() == 2);
    std::swap(p.crossfades[0], p.crossfades[1]);
    std::string where_all;
    const std::string all = detail::playlist_problem(p, *s.sources, 1, where_all);
    CHECK(all == detail::kCrossfadesUnsorted);
    CHECK(where_all == "crossfades[1]");

    // Regions 3 to 5 are watched, so crossfades[0] is and crossfades[1] is not.
    detail::Touched touched;
    touched.mark(4, 4);
    std::string where_near;
    CHECK(detail::playlist_problem_near(p, *s.sources, 1, touched, where_near) == all);
    CHECK(where_near == where_all);
    return 0;
}

// Random compounds on dense crossfaded playlists. The audited library checks
// every member as it is folded, and every member applied alone; here, the
// fold must also equal the members one by one, and a compound with a
// refused member must be refused with that member's own reason.
int test_random_compounds() {
    Gen g(20261018);
    Session base;
    CHECK(build(g, base) == 0);
    CHECK(crossfades_in(base.s) > 40);
    Snapshot cur = base.s;
    int folded = 0, refused = 0, members = 0, member_refusals = 0;
    std::size_t most_crossfades = 0;
    for (int trial = 0; trial < 1500; ++trial) {
        Compound c;
        Snapshot s = cur;
        std::string member_error;
        const std::uint64_t length = 1 + g.below(48);
        for (std::uint64_t j = 0; j < length; ++j) {
            Member m;
            CHECK(to_member(next_member(s, base.tracks, g), m));
            Snapshot next;
            std::string err;
            if (apply(s, to_operation(m), next, err)) {
                c.ops.push_back(m);
                s = std::move(next);
                ++members;
                continue;
            }
            ++member_refusals;
            if (g.chance(15)) {
                c.ops.push_back(m);
                member_error = err;
                break;
            }
        }
        if (c.ops.empty()) continue;
        Snapshot after;
        std::string err;
        const bool ok = apply(cur, Operation{c}, after, err);
        if (!member_error.empty()) {
            CHECK(!ok);
            CHECK(mentions(err, member_error.c_str()));
            ++refused;
            continue;
        }
        if (!ok) std::fprintf(stderr, "  refused: %s\n", err.c_str());
        CHECK(ok);
        CHECK(*after.edit == *s.edit);
        CHECK(after.next_id == s.next_id);
        CHECK(invariants_hold(after));
        ++folded;
        most_crossfades = std::max(most_crossfades, crossfades_in(after));
        // Keep going from the result, so later compounds meet what earlier
        // ones left; start over before the playlists drift far from dense.
        cur = trial % 100 == 99 ? base.s : after;
    }
    std::printf("model_local_edit: %d compounds folded, %d refused, %d members applied, %d"
                " member refusals, up to %zu crossfades\n",
                folded, refused, members, member_refusals, most_crossfades);
    // A generator that only produced refusals would prove nothing.
    CHECK(folded > 900);
    CHECK(refused > 100);
    CHECK(members > 15000);
    CHECK(member_refusals > 1000);
    return 0;
}

}  // namespace

int main() {
    // Without the audit this binary would check far less than it claims.
    CHECK(sonora::model::detail::audited());
    CHECK(test_a_move_fits_the_fade_it_uncovers() == 0);
    CHECK(test_a_trim_drops_the_crossfade_that_no_longer_fits() == 0);
    CHECK(test_a_cut_keeps_the_crossfades_beside_it() == 0);
    CHECK(test_a_watched_crossfade_before_an_earlier_one_is_unsorted() == 0);
    CHECK(test_random_compounds() == 0);
    return 0;
}
