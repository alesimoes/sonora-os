// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property test: a long random edit sequence, fixed seed, checking after
// every step that
//   - every invariant of the session format holds;
//   - a refused operation changed nothing;
//   - undo right after an edit gives back the exact previous edit state, and
//     redo the exact next one;
//   - a replica fed the same messages and floors ends equal to the engine;
//   - the final state survives serialize -> deserialize as the same
//     document, and saves to the same bytes again.
// The generator draws from the snapshot itself, so most operations name
// real regions and many succeed; the rest exercise the refusals.
#include <cstdint>
#include <random>

#include "model_test.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

class Gen {
public:
    explicit Gen(std::uint64_t seed) : rng_(seed) {}
    // rng() % n rather than a std distribution: the distributions are not
    // specified bit-for-bit, and the sequence must be the same everywhere.
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : rng_() % n; }
    Frames frames(Frames lo, Frames hi) {
        return lo + static_cast<Frames>(below(static_cast<std::uint64_t>(hi - lo + 1)));
    }
    bool chance(int percent) { return below(100) < static_cast<std::uint64_t>(percent); }

private:
    std::mt19937_64 rng_;
};

const Playlist& playlist_of_track(const TrackEdit& t) {
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return *p;
    }
    return *t.playlists.front();
}

struct Picked {
    const TrackEdit* track = nullptr;
    const Playlist* playlist = nullptr;
    const Region* region = nullptr;
};

Picked pick(const Snapshot& s, Gen& g) {
    Picked p;
    const auto& tracks = s.edit->tracks;
    if (tracks.empty()) return p;
    p.track = tracks[g.below(tracks.size())].get();
    const auto& pls = p.track->playlists;
    p.playlist = g.chance(80) ? &playlist_of_track(*p.track) : pls[g.below(pls.size())].get();
    if (!p.playlist->regions.empty()) {
        p.region = &p.playlist->regions[g.below(p.playlist->regions.size())];
    }
    return p;
}

const Source* source_for(const Snapshot& s, std::uint16_t channels, Gen& g) {
    std::vector<const Source*> fit;
    for (const Source& src : s.sources->sources) {
        if (src.channels == channels) fit.push_back(&src);
    }
    return fit.empty() ? nullptr : fit[g.below(fit.size())];
}

Region random_region(Id id, const Source& src, std::uint16_t channels, Gen& g) {
    Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.position = g.frames(0, 2'000'000);
    r.source_offset = g.frames(0, src.length / 2);
    r.length = g.frames(1, std::max<Frames>(1, src.length - r.source_offset));
    for (std::uint16_t c = 0; c < channels; ++c) r.channels.push_back({src.id, c});
    r.gain_db = static_cast<float>(g.frames(-60, 6)) / 4.0f;
    if (g.chance(20)) r.fade_in = Fade{g.frames(0, r.length / 2), FadeShape::SCurve};
    return r;
}

Crossfade random_xfade(Id left, Id right, Gen& g) {
    static const Align aligns[] = {Align::Centre, Align::Pre, Align::Post};
    return Crossfade{left, right, g.frames(1, 2000), aligns[g.below(3)],
                     FadeShape::EqualPower};
}

// One operation the engine might resolve from a command.
Operation next_op(const Snapshot& s, Gen& g) {
    Id next = s.next_id;
    const Picked p = pick(s, g);
    const std::uint64_t kind = g.below(16);
    if (p.track == nullptr || (kind == 0 && s.edit->tracks.size() < 5)) {
        AddTrack t;
        t.track = next++;
        t.playlist = next++;
        t.name = "T" + std::to_string(t.track);
        t.playlist_name = t.name + ".01";
        t.channels = static_cast<std::uint16_t>(1 + g.below(2));
        for (std::uint16_t c = 0; c < t.channels; ++c) t.mix.input.push_back(c);
        return t;
    }
    const std::uint16_t ch = p.track->channels;
    const Id pl = p.playlist->id;
    switch (kind) {
        case 1: {
            AddSource a;
            a.source.id = next;
            a.source.path = "Audio Files/s" + std::to_string(next) + ".wav";
            a.source.channels = ch;
            a.source.length = g.frames(1000, 400'000);
            a.source.take = static_cast<std::int64_t>(next);
            a.source.recorded_on_track = p.track->id;
            return a;
        }
        case 2:
        case 3: {
            const Source* src = source_for(s, ch, g);
            if (src == nullptr) break;
            RecordTake op;
            op.track = p.track->id;
            op.playlist = pl;
            op.region = random_region(next++, *src, ch, g);
            op.split_right_id = next++;
            // A punch's crossfades join the take to whatever ends up beside
            // it; when they cannot, apply refuses and that is tested too.
            if (g.chance(60)) {
                for (const Region& r : p.playlist->regions) {
                    if (r.position < op.region.position && r.end() > op.region.position) {
                        op.crossfades.push_back(random_xfade(r.id, op.region.id, g));
                    }
                }
            }
            return op;
        }
        case 4: {
            const Source* src = source_for(s, ch, g);
            if (src == nullptr) break;
            return PlaceRegion{pl, random_region(next, *src, ch, g), next + 1};
        }
        case 5:
            if (p.region == nullptr) break;
            return MoveRegion{pl, p.region->id, g.frames(0, 2'000'000), next};
        case 6:
            if (p.region == nullptr) break;
            return TrimRegion{pl, p.region->id, g.chance(50) ? Edge::Start : Edge::End,
                              p.region->position + g.frames(-50'000, p.region->length + 50'000)};
        case 7:
            if (p.region == nullptr) break;
            return SplitRegion{pl, p.region->id,
                               p.region->position + g.frames(0, p.region->length), next};
        case 8:
            if (p.region == nullptr) break;
            return RemoveRegion{pl, p.region->id};
        case 9:
            if (p.region == nullptr) break;
            return SetFade{pl, p.region->id, g.chance(50) ? FadeEnd::In : FadeEnd::Out,
                           Fade{g.frames(0, p.region->length), FadeShape::Linear}};
        case 10: {
            const auto& rs = p.playlist->regions;
            for (std::size_t i = 1; i < rs.size(); ++i) {
                if (rs[i - 1].end() == rs[i].position && g.chance(50)) {
                    return SetCrossfade{pl, random_xfade(rs[i - 1].id, rs[i].id, g)};
                }
            }
            if (!p.playlist->crossfades.empty()) {
                const Crossfade& x = p.playlist->crossfades[g.below(p.playlist->crossfades.size())];
                return RemoveCrossfade{pl, x.left, x.right};
            }
            break;
        }
        case 11:
            if (g.chance(30)) return AddPlaylist{p.track->id, next, "alt"};
            return SetActivePlaylist{p.track->id, p.track->playlists[g.below(p.track->playlists.size())]->id};
        case 12: {
            const auto& ms = s.edit->markers->markers;
            if (ms.empty() || g.chance(40)) {
                return AddMarker{Marker{next, g.frames(0, 2'000'000), "m", g.frames(0, 1000)}};
            }
            const Id m = ms[g.below(ms.size())].id;
            if (g.chance(50)) return MoveMarker{m, g.frames(-10, 2'000'000)};
            return RemoveMarker{m};
        }
        case 13: {
            TrackMix m = *s.mix.find(p.track->id);
            m.pan = static_cast<float>(g.frames(-6, 6)) / 5.0f;   // sometimes out of range
            m.gain_db = static_cast<float>(g.frames(-100, 10)) / 3.0f;
            return SetMix{m};
        }
        case 14: {
            ReorderTracks r;
            for (const auto& t : s.edit->tracks) r.order.push_back(t->id);
            if (r.order.size() > 1) std::swap(r.order[0], r.order[g.below(r.order.size())]);
            return r;
        }
        default:
            return SetTrackName{p.track->id, "n" + std::to_string(g.below(1000))};
    }
    return SetTrackName{p.track->id, "fallback"};
}

}  // namespace

int main() {
    Gen g(20261013);
    History engine(new_session(settings()), 256 * 1024);
    History replica = History::replica(engine.current());
    int applied = 0, refused = 0, undone = 0, evictions = 0;
    std::uint64_t floor = engine.floor();

    const auto follow = [&](int kind, const Operation* op) {
        // kind: 0 op, 1 undo, 2 redo. What the engine would send.
        std::string err;
        bool ok = true;
        if (kind == 0) ok = replica.apply(*op, err);
        if (kind == 1) ok = replica.undo();
        if (kind == 2) ok = replica.redo();
        replica.evict_below(engine.floor());
        return ok && replica.current().revision == engine.current().revision;
    };

    for (int step = 0; step < 4000; ++step) {
        if (g.chance(8) && engine.can_undo()) {
            CHECK(engine.undo());
            CHECK(follow(1, nullptr));
            CHECK(invariants_hold(engine.current()));
            continue;
        }
        if (g.chance(4) && engine.can_redo()) {
            CHECK(engine.redo());
            CHECK(follow(2, nullptr));
            CHECK(invariants_hold(engine.current()));
            continue;
        }
        const Operation op = next_op(engine.current(), g);
        const Snapshot before = engine.current();
        std::string err;
        if (!engine.apply(op, err)) {
            ++refused;
            CHECK(engine.current() == before);
            continue;
        }
        ++applied;
        CHECK(follow(0, &op));
        const Snapshot& after = engine.current();
        if (!invariants_hold(after)) {
            std::fprintf(stderr, "  after step %d (operation %zu)\n", step, op.index());
            return 1;
        }
        CHECK(after.revision == before.revision + 1);
        if (!is_undoable(op)) {
            CHECK(after.edit == before.edit);
            continue;
        }
        // Undo restores the very value, and redo the edit just made.
        if (g.chance(25)) {
            const auto made = after.edit;
            CHECK(engine.undo());
            CHECK(follow(1, nullptr));
            CHECK(*engine.current().edit == *before.edit);
            CHECK(engine.current().sources == before.sources);
            CHECK(engine.redo());
            CHECK(follow(2, nullptr));
            CHECK(engine.current().edit == made);
            ++undone;
        }
        if (engine.floor() != floor) ++evictions;
        floor = engine.floor();
    }

    CHECK(replica.current() == engine.current());
    CHECK(replica.floor() == engine.floor());
    CHECK(replica.step_count() == engine.step_count());

    std::string bytes, err;
    CHECK(serialize(engine.current(), WriteInfo{"random", 1}, bytes, err));
    Loaded l;
    LoadError e;
    if (!deserialize(bytes, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(deserialize(bytes, l, e));
    CHECK(same_document(l.snapshot, engine.current()));
    std::string again;
    CHECK(serialize(l.snapshot, WriteInfo{"random", 1}, again, err));
    CHECK(again == bytes);

    std::size_t regions = 0;
    for (const auto& t : engine.current().edit->tracks) {
        for (const auto& p : t->playlists) regions += p->regions.size();
    }
    // A generator that only produced refusals would prove nothing.
    CHECK(applied > 1500);
    CHECK(undone > 100);
    CHECK(evictions > 0);
    CHECK(regions > 20);
    std::printf("model_random_edits: 4000 steps, %d applied, %d refused, %d undo/redo"
                " checks, %d evictions, %zu regions, %zu bytes saved\n",
                applied, refused, undone, evictions, regions, bytes.size());
    return 0;
}
