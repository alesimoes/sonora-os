// SPDX-License-Identifier: GPL-3.0-or-later
//
// A local stand-in for `common/model` (ADR-0007 M8), shaped to
// brain/specs/timeline-model.md so that the real one replaces it by a
// namespace and an include, not a redesign. Nothing here is Qt; nothing here
// is the engine's. When common/model lands (feature/timeline-model), this
// file and timeline_model.cpp are deleted and `sonora::ui::model` becomes an
// alias of `sonora::model`. Every name below is the spec's name.
//
// What is the same as the spec:
//   - value types; an EditState is never modified, an edit builds a new one
//     that shares every track it did not touch (shared_ptr<const ...>);
//   - operations are plain, fully resolved values, and `apply` is a pure
//     function of (snapshot, operation): it allocates no id, reads no clock;
//   - playlists are sorted and never overlap; moving a region over another
//     trims, splits or removes what is beneath in the same operation;
//   - mix state and settings are outside EditState and so outside undo.
//
// What is reduced, and says so:
//   - one playlist per track is used (the type carries the list);
//   - no crossfades, markers or fades are edited yet (the fields exist);
//   - the source pool holds only what bounds a trim: id, name, length.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace sonora::ui::model {

using Id = uint64_t;
using Frames = int64_t;

constexpr Id kNoId = 0;

struct SourceRef {
    Id source = kNoId;
    uint16_t channel = 0;
};

enum class FadeShape { Linear, EqualPower, SCurve };

struct Fade {
    Frames length = 0;
    FadeShape shape = FadeShape::Linear;
};

struct Region {
    Id id = kNoId;
    std::string name;
    Frames position = 0;
    Frames length = 0;
    Frames source_offset = 0;
    std::vector<SourceRef> channels;  // one per track channel
    float gain_db = 0.0f;
    bool mute = false;
    Fade fade_in, fade_out;

    Frames end() const { return position + length; }
    bool operator==(const Region& o) const;
};

struct Playlist {
    Id id = kNoId;
    std::string name;
    std::vector<Region> regions;  // sorted by position, never overlapping
};

struct TrackEdit {
    Id id = kNoId;
    std::string name;
    uint16_t channels = 1;
    Id active_playlist = kNoId;
    std::vector<std::shared_ptr<const Playlist>> playlists;

    const Playlist* active() const;
};

struct EditState {
    std::vector<std::shared_ptr<const TrackEdit>> tracks;  // display order
};

struct Source {
    Id id = kNoId;
    std::string name;
    uint16_t channels = 1;
    Frames length = 0;
};

struct SourcePool {
    std::vector<Source> sources;  // append-only
    const Source* find(Id id) const;
};

// Mix state: saved, not undoable (spec, "What undo covers").
struct TrackMix {
    Id track = kNoId;
    float gain_db = 0.0f;
    float pan = 0.0f;  // -1 left .. +1 right
    bool mute = false;
    bool solo = false;
};

struct MonitorBus {
    uint16_t outputs[2] = {0, 1};
    float gain_db = 0.0f;
    float pan_law_db = -3.0f;
};

struct MixState {
    std::vector<TrackMix> tracks;  // same order as EditState::tracks
    MonitorBus bus;
    const TrackMix* find(Id track) const;
};

// session-format.md, "timecode", "tempo", "transport".
struct SessionSettings {
    uint32_t sample_rate = 48000;
    // Sample 0 of the timeline as samples since midnight. 01:00:00:00.
    int64_t timecode_origin = 3600LL * 48000;
    // "23.976" "24" "25" "29.97" "29.97df" "30"; display only.
    std::string fps = "25";
    double bpm = 120.0;
    int meter_num = 4, meter_den = 4;
    Frames loop[2] = {0, 0};   // [0,0] is unset
    Frames punch[2] = {0, 0};
};

struct Snapshot {
    std::shared_ptr<const EditState> edit;
    std::shared_ptr<const SourcePool> sources;
    MixState mix;
    SessionSettings settings;
    uint64_t revision = 0;

    const TrackEdit* track(Id id) const;
    int track_index(Id id) const;  // display order, or -1
    // The region and the track that holds it in its active playlist.
    const Region* find_region(Id region, const TrackEdit** track = nullptr) const;
};

// --- operations (spec, "Operations Phase 2 needs": the editing subset) ---

enum class Edge { Start, End };

struct MoveRegion {
    Id playlist = kNoId;
    Id region = kNoId;
    Frames to = 0;
    // The id the right half of a region the move lands inside gets when it
    // is split in two (spec, "Overwrite"). The resolver reserves it; apply
    // never allocates one.
    Id split_id = kNoId;
};

struct TrimRegion {
    Id playlist = kNoId;
    Id region = kNoId;
    Edge edge = Edge::Start;
    Frames to = 0;  // the new timeline position of that edge
};

struct SplitRegion {
    Id playlist = kNoId;
    Id region = kNoId;
    Frames at = 0;
    Id new_id = kNoId;
};

struct RemoveRegion {
    Id playlist = kNoId;
    Id region = kNoId;
};

enum class MixField { GainDb, Pan, Mute, Solo, BusGainDb };

struct SetMix {
    Id track = kNoId;  // ignored for BusGainDb
    MixField field = MixField::GainDb;
    float value = 0.0f;  // booleans as 0 / 1
};

using Operation = std::variant<MoveRegion, TrimRegion, SplitRegion, RemoveRegion, SetMix>;

// Whether `op` is an edit (an undo step) or mix state (not undoable).
bool undoable(const Operation& op);
const char* operation_name(const Operation& op);

// The spec's `Snapshot apply(const Snapshot&, const Operation&)`, or an
// error: on failure `why` says what was refused and `out` is untouched.
// Pure. Shares every part of `before` the operation does not change.
bool apply(const Snapshot& before, const Operation& op, Snapshot& out, std::string& why);

// Checks the invariants session-format.md says are checked at load: sorted,
// non-overlapping, length > 0, inside the source. Empty when the snapshot is
// valid. For tests, and for the stand-in to assert on itself.
std::string check_invariants(const Snapshot& s);

// Gain bounds a fader can reach (ui-design.md, "Fader").
constexpr float kMaxGainDb = 12.0f;
constexpr float kMinGainDb = -144.0f;  // "-inf" on screen

}  // namespace sonora::ui::model
