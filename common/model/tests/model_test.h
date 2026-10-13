// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared by common/model's tests: the CHECK macro and a small builder for
// sessions with real ids, so each test says what it is about rather than how
// a track is assembled.
#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/model.h"
#include "sonora/model/operations.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

// Like CHECK, for an apply that must succeed; prints apply's reason.
#define CHECK_APPLY(history, op)                                              \
    do {                                                                      \
        std::string check_apply_error_;                                       \
        if (!(history).apply((op), check_apply_error_)) {                     \
            std::fprintf(stderr, "FAIL %s:%d: apply refused: %s\n", __FILE__, \
                         __LINE__, check_apply_error_.c_str());               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace model_test {

using namespace sonora::model;

inline bool mentions(const std::string& haystack, const char* needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle, haystack.c_str());
    return false;
}

inline SessionSettings settings() {
    SessionSettings s;
    s.name = "Test";
    return s;
}

// Hands out ids the way the engine's counter does: from next_id upwards.
struct Ids {
    Id next = 1;
    explicit Ids(const Snapshot& s) : next(s.next_id) {}
    Id operator()() { return next++; }
};

inline AddTrack add_track(Ids& ids, const std::string& name, std::uint16_t channels = 1) {
    AddTrack op;
    op.track = ids();
    op.playlist = ids();
    op.name = name;
    op.playlist_name = name + ".01";
    op.channels = channels;
    for (std::uint16_t c = 0; c < channels; ++c) op.mix.input.push_back(c);
    return op;
}

inline AddSource add_source(Ids& ids, Frames length, std::uint16_t channels = 1,
                            Id track = 0) {
    AddSource op;
    op.source.id = ids();
    op.source.path = "Audio Files/take_" + std::to_string(op.source.id) + ".wav";
    op.source.channels = channels;
    op.source.length = length;
    op.source.kind = SourceKind::Recorded;
    op.source.take = static_cast<std::int64_t>(op.source.id);
    op.source.recorded_on_track = track;
    return op;
}

inline Region region(Id id, Id source, Frames position, Frames length,
                     Frames source_offset = 0, std::uint16_t channels = 1) {
    Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.position = position;
    r.length = length;
    r.source_offset = source_offset;
    for (std::uint16_t c = 0; c < channels; ++c) r.channels.push_back({source, c});
    return r;
}

inline const Playlist& playlist_of(const Snapshot& s, std::size_t track) {
    const TrackEdit& t = *s.edit->tracks[track];
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return *p;
    }
    return *t.playlists.front();
}

inline const Region* find(const Playlist& p, Id id) {
    for (const Region& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

inline bool invariants_hold(const Snapshot& s) {
    std::string e;
    if (check_invariants(s, e)) return true;
    std::fprintf(stderr, "  invariant broken: %s\n", e.c_str());
    return false;
}

}  // namespace model_test
