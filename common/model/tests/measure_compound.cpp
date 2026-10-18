// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a compound costs the document thread (ADR-0009 update 2026-10-14,
// item 5): kMaxCompoundMembers members on one 2,000-region playlist, and on
// 18 tracks of 2,000 regions, against the same members applied one by one. Run by hand;
// the numbers go in the ADR. Not a test: it asserts nothing about time.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/operations.h"
#include "sonora/model/replication.h"

using namespace sonora::model;

namespace {

constexpr Frames kRegion = 1000;
constexpr Frames kPitch = 2000;   // a gap after every region

struct Session {
    Snapshot snapshot;
    std::vector<Id> playlists;
    std::vector<std::vector<Id>> regions;   // per track, in position order
};

bool build(int tracks, int regions, Session& out) {
    SessionSettings settings;
    settings.name = "Measure";
    Snapshot s = new_session(settings);
    std::string err;
    const auto step = [&](const Operation& op) {
        Snapshot next;
        if (!apply(s, op, next, err)) {
            std::fprintf(stderr, "setup refused: %s\n", err.c_str());
            return false;
        }
        s = std::move(next);
        return true;
    };
    for (int t = 0; t < tracks; ++t) {
        AddTrack at;
        at.track = s.next_id;
        at.playlist = s.next_id + 1;
        at.name = "T" + std::to_string(t + 1);
        at.playlist_name = at.name + ".01";
        at.mix.input = {static_cast<std::uint16_t>(t)};
        if (!step(at)) return false;
        AddSource src;
        src.source.id = s.next_id;
        src.source.path = "Audio Files/T" + std::to_string(t + 1) + ".wav";
        src.source.channels = 1;
        src.source.length = kPitch * regions + kPitch;
        if (!step(src)) return false;
        out.playlists.push_back(at.playlist);
        out.regions.emplace_back();
        // Placed as one playlist value rather than 2,000 operations, so the
        // setup is not itself the quadratic case being measured.
        Playlist p = *s.edit->tracks.back()->playlists[0];
        for (int i = 0; i < regions; ++i) {
            Region r;
            r.id = s.next_id++;
            r.name = "r" + std::to_string(r.id);
            r.position = kPitch * i;
            r.length = kRegion;
            r.source_offset = kPitch * i;
            r.channels = {{src.source.id, 0}};
            p.regions.push_back(r);
            out.regions.back().push_back(r.id);
        }
        auto track = std::make_shared<TrackEdit>(*s.edit->tracks.back());
        track->playlists[0] = std::make_shared<Playlist>(std::move(p));
        auto edit = std::make_shared<EditState>(*s.edit);
        edit->tracks.back() = std::move(track);
        s.edit = std::move(edit);
    }
    if (!check_invariants(s, err)) {
        std::fprintf(stderr, "setup broke an invariant: %s\n", err.c_str());
        return false;
    }
    out.snapshot = std::move(s);
    return true;
}

// The shape a cut makes: split a region, then remove its right part, over
// and over, spread round-robin across the tracks: two members a pair.
Compound cut_like(const Session& s, std::size_t members) {
    Compound c;
    c.label = "Cut";
    Id next = s.snapshot.next_id;
    std::size_t k = 0;
    while (c.ops.size() + 2 <= members) {
        const std::size_t t = k % s.playlists.size();
        const std::size_t i = (k / s.playlists.size()) % s.regions[t].size();
        // A second pass over a region splits what the first left of it.
        const auto pass = static_cast<Frames>(k / (s.playlists.size() * s.regions[t].size()));
        const Id region = s.regions[t][i];
        const Frames at = kPitch * static_cast<Frames>(i) + kRegion / (2 + pass);
        const Id right = next++;
        c.ops.push_back(SplitRegion{s.playlists[t], region, at, right});
        c.ops.push_back(RemoveRegion{s.playlists[t], right});
        ++k;
    }
    return c;
}

// Fade edits only: nothing is added or removed, so every member is the same
// work on a playlist of constant size.
Compound fades(const Session& s, std::size_t members) {
    Compound c;
    for (std::size_t k = 0; k < members; ++k) {
        const std::size_t t = k % s.playlists.size();
        const std::size_t i = (k / s.playlists.size()) % s.regions[t].size();
        const Frames len = 10 + static_cast<Frames>(k % 50);
        c.ops.push_back(SetFade{s.playlists[t], s.regions[t][i], FadeEnd::In,
                                Fade{len, FadeShape::Linear}});
    }
    return c;
}

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Timing {
    double best = 1e300;
    double median = 0;
};

template <typename F>
Timing time_runs(int runs, F&& f) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        const auto start = Clock::now();
        if (!f()) return Timing{-1, -1};
        t.push_back(ms_since(start));
    }
    std::sort(t.begin(), t.end());
    return Timing{t.front(), t[t.size() / 2]};
}

bool measure(const char* what, int tracks, int regions, const Compound& c, int runs) {
    Session s;
    if (!build(tracks, regions, s)) return false;
    std::string err;

    // The compound through the Document, as the engine applies it: encode,
    // fold, one history step.
    const Timing whole = time_runs(runs, [&] {
        Document doc(s.snapshot);
        ReplicationUpdate u;
        if (!doc.apply(Operation{c}, u, err)) {
            std::fprintf(stderr, "%s refused: %s\n", what, err.c_str());
            return false;
        }
        return true;
    });
    // The fold alone, without the codec.
    const Timing fold_only = time_runs(runs, [&] {
        Snapshot after;
        if (!apply(s.snapshot, Operation{c}, after, err)) return false;
        return true;
    });
    // The same members one by one, each its own operation and step.
    const Timing one_by_one = time_runs(runs, [&] {
        Document doc(s.snapshot);
        ReplicationUpdate u;
        for (const Member& m : c.ops) {
            if (!doc.apply(to_operation(m), u, err)) {
                std::fprintf(stderr, "%s one by one refused: %s\n", what, err.c_str());
                return false;
            }
        }
        return true;
    });
    std::string bytes;
    encode_operation(Operation{c}, bytes, err);
    const Timing decode = time_runs(runs, [&] {
        Operation op;
        return decode_operation(bytes, op, err);
    });
    if (whole.best < 0 || fold_only.best < 0 || one_by_one.best < 0 || decode.best < 0) {
        return false;
    }
    std::printf(
        "%-44s %5zu members  %2d x %4d regions  Document::apply %8.2f ms (median %8.2f)  "
        "fold %8.2f ms  decode %6.2f ms  one by one %9.2f ms  %7zu bytes\n",
        what, c.ops.size(), tracks, regions, whole.best, whole.median, fold_only.best,
        decode.best, one_by_one.best, bytes.size());
    return true;
}

}  // namespace

int main() {
    std::printf("measure_compound: best of 5 runs, steady_clock\n");
    bool ok = true;
    for (const int tracks : {1, 18}) {
        Session shape;
        if (!build(tracks, 2000, shape)) return 1;
        ok = measure(tracks == 1 ? "cut-like (split+remove), one playlist"
                                 : "cut-like (split+remove), 18 playlists",
                     tracks, 2000, cut_like(shape, kMaxCompoundMembers), 5) && ok;
        ok = measure(tracks == 1 ? "SetFade, one playlist" : "SetFade, 18 playlists", tracks,
                     2000, fades(shape, kMaxCompoundMembers), 5) && ok;
    }
    // How the cost grows with the member count, below the limit.
    for (const std::size_t n : {std::size_t{1}, std::size_t{16}, std::size_t{32}}) {
        Session shape;
        if (!build(1, 2000, shape)) return 1;
        ok = measure("SetFade, one playlist", 1, 2000, fades(shape, n), 5) && ok;
    }
    return ok ? 0 : 1;
}
