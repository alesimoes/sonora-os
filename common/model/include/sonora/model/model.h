// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session document (brain/specs/timeline-model.md, ADR-0009).
//
// Never on the audio thread (ADR-0007 §2): these types allocate, share
// ownership and hold strings. The audio thread sees only the plan that
// engine/control builds from a Snapshot. Standard C++17 only (ADR-0008): no
// file, no clock, no thread.
//
// Everything reachable from a Snapshot is immutable once built. An edit
// builds a new EditState that shares every track and playlist it did not
// touch with the one before, which is what makes undo a pointer and lets a
// saver or a plan builder read a snapshot while the next edit is applied.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora::model {

using Id = std::uint64_t;      // one counter for every object; 0 is "none"
using Frames = std::int64_t;   // samples at the session rate, never negative

// Integers in the session file stay inside what jq, Python and JavaScript
// read exactly (session-format.md, "Numbers").
constexpr std::int64_t kMaxInteger = (std::int64_t{1} << 53) - 1;

enum class FadeShape : std::uint8_t { Linear, EqualPower, SCurve };
enum class Align : std::uint8_t { Centre, Pre, Post };
enum class AudioFormat : std::uint8_t { Int24, Float32 };
enum class SourceKind : std::uint8_t { Recorded, Imported };
enum class MonitorMode : std::uint8_t { Auto, Input, Off };
enum class Fps : std::uint8_t { F23_976, F24, F25, F29_97, F29_97df, F30 };

struct SourceRef {
    Id source = 0;
    std::uint16_t channel = 0;
    bool operator==(const SourceRef& o) const {
        return source == o.source && channel == o.channel;
    }
};

struct Fade {
    Frames length = 0;
    FadeShape shape = FadeShape::Linear;
    bool operator==(const Fade& o) const {
        return length == o.length && shape == o.shape;
    }
};

// Every gain the document holds (track, bus, region), in dB. The audio
// thread multiplies by 10^(dB/20) with no check of its own, so the range is
// what keeps inf (and the NaN that inf times silence makes) and subnormals
// out of it: the ends are about 15.8 and 6.3e-8.
inline constexpr float kMinGainDb = -144.0f;
inline constexpr float kMaxGainDb = 24.0f;
// The monitor bus's pan law at centre, in dB: 0 (balance) down to this. A
// positive law is a negative exponent, which is inf at a hard pan.
inline constexpr float kMinPanLawDb = -12.0f;

struct Region {
    Id id = 0;
    std::string name;
    Frames position = 0;       // timeline frame of the region's first frame
    Frames length = 0;         // > 0
    Frames source_offset = 0;  // the source frame that plays at `position`
    std::vector<SourceRef> channels;  // one per track channel
    float gain_db = 0.0f;
    bool mute = false;
    Fade fade_in, fade_out;

    Frames end() const { return position + length; }
    bool operator==(const Region& o) const;
};

// Joins two regions that butt on one playlist, drawing on each one's handle
// (session-format.md, "Crossfades"). Named by its two regions; it has no id.
struct Crossfade {
    Id left = 0;
    Id right = 0;
    Frames length = 0;
    Align align = Align::Centre;
    FadeShape shape = FadeShape::EqualPower;
    bool operator==(const Crossfade& o) const {
        return left == o.left && right == o.right && length == o.length &&
               align == o.align && shape == o.shape;
    }
};

// The span a crossfade covers either side of the cut: it runs over
// [cut - before, cut + after). An odd centred length puts the extra frame
// after the cut.
void crossfade_extent(const Crossfade& x, Frames& before, Frames& after);

struct Playlist {
    Id id = 0;
    std::string name;
    std::vector<Region> regions;         // sorted by position, never overlapping
    std::vector<Crossfade> crossfades;   // sorted by the left region's position
    bool operator==(const Playlist& o) const;
};

// The undoable part of a track. Input, monitor mode and the mix live in
// TrackMix, because a fader move is not an edit.
struct TrackEdit {
    Id id = 0;
    std::string name;
    std::uint16_t channels = 1;
    Id active_playlist = 0;
    std::vector<std::shared_ptr<const Playlist>> playlists;
    bool operator==(const TrackEdit& o) const;
};

struct Marker {
    Id id = 0;
    Frames position = 0;
    std::string name;
    Frames length = 0;   // > 0 makes it a range
    bool operator==(const Marker& o) const {
        return id == o.id && position == o.position && name == o.name &&
               length == o.length;
    }
};

struct MarkerList {
    std::vector<Marker> markers;   // sorted by (position, id)
};

// One undo step's worth of state.
struct EditState {
    std::vector<std::shared_ptr<const TrackEdit>> tracks;   // display order
    std::shared_ptr<const MarkerList> markers;
    bool operator==(const EditState& o) const;
};

struct Source {
    Id id = 0;
    std::string path;           // relative to the session folder, '/'-separated
    std::uint16_t channels = 1;
    Frames length = 0;
    AudioFormat format = AudioFormat::Int24;
    std::int64_t time_reference = 0;
    std::string umid;           // 64 lower-case hex digits, or empty if none
    SourceKind kind = SourceKind::Recorded;
    // Recorded sources only.
    std::int64_t take = 0;
    Id recorded_on_track = 0;
    Frames latency_compensation = 0;
    bool operator==(const Source& o) const;
};

// Append-only: undo never removes a source (session-format.md, rule 2).
struct SourcePool {
    std::vector<Source> sources;   // sorted by id
    const Source* find(Id id) const;
};

struct TrackMix {
    Id track = 0;
    std::vector<std::uint16_t> input;   // one capture channel per track channel
    MonitorMode monitor = MonitorMode::Auto;
    float gain_db = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    bool operator==(const TrackMix& o) const;
};

struct MonitorBus {
    std::array<std::uint16_t, 2> outputs{{0, 1}};
    float gain_db = 0.0f;
    float pan_law_db = -3.0f;
    bool operator==(const MonitorBus& o) const {
        return outputs == o.outputs && gain_db == o.gain_db &&
               pan_law_db == o.pan_law_db;
    }
};

// Saved, never undone.
struct MixState {
    MonitorBus bus;
    // Keyed by track id. An entry outlives an undone AddTrack, so redo
    // brings the track back with its faders where they were.
    std::vector<TrackMix> tracks;
    const TrackMix* find(Id track) const;
    bool operator==(const MixState& o) const {
        return bus == o.bus && tracks == o.tracks;
    }
};

struct Timecode {
    std::int64_t origin = 172'800'000;   // 01:00:00:00 at 48 kHz
    Fps fps = Fps::F25;
    bool operator==(const Timecode& o) const {
        return origin == o.origin && fps == o.fps;
    }
};

struct TempoEntry {
    Frames at = 0;
    float bpm = 120.0f;
    std::array<std::uint32_t, 2> meter{{4, 4}};
    bool operator==(const TempoEntry& o) const {
        return at == o.at && bpm == o.bpm && meter == o.meter;
    }
};

struct Transport {
    std::array<Frames, 2> loop{{0, 0}};    // [0, 0] is unset
    std::array<Frames, 2> punch{{0, 0}};
    Frames pre_roll = 96000;
    Frames post_roll = 48000;
    Frames punch_crossfade = 240;
    bool operator==(const Transport& o) const {
        return loop == o.loop && punch == o.punch && pre_roll == o.pre_roll &&
               post_roll == o.post_roll && punch_crossfade == o.punch_crossfade;
    }
};

struct SessionSettings {
    std::string name;
    std::uint32_t sample_rate = 48000;
    AudioFormat audio_format = AudioFormat::Int24;
    Timecode timecode;
    std::vector<TempoEntry> tempo{TempoEntry{}};
    Transport transport;
    bool operator==(const SessionSettings& o) const {
        return name == o.name && sample_rate == o.sample_rate &&
               audio_format == o.audio_format && timecode == o.timecode &&
               tempo == o.tempo && transport == o.transport;
    }
};

// What a save, a plan build or a replica reads.
struct Snapshot {
    std::shared_ptr<const EditState> edit;
    std::shared_ptr<const SourcePool> sources;
    MixState mix;
    SessionSettings settings;
    // Bumped by every applied operation, undo and redo. A replica checks
    // each message's revision is the next one.
    std::uint64_t revision = 0;
    // Every id an operation introduces must be at least this, which makes
    // uniqueness an O(1) check. Not saved: a load resumes at max(id) + 1.
    Id next_id = 1;
};

// An empty session with these settings: no track, no source, no marker.
Snapshot new_session(const SessionSettings& settings);

// Everything a replica must agree on, revision and id counter included.
bool operator==(const Snapshot& a, const Snapshot& b);
inline bool operator!=(const Snapshot& a, const Snapshot& b) { return !(a == b); }

// What the session file holds: the edit, the pool, the settings and the
// mix of the tracks that exist. Not the revision, the id counter, or a mix
// entry left behind by an undone AddTrack.
bool same_document(const Snapshot& a, const Snapshot& b);

// Every invariant of the session format (session-format.md) over the whole
// snapshot. On failure `error` names the object by its key path, as the
// file would ("tracks[1].playlists[0].regions[3]: ...").
bool check_invariants(const Snapshot& s, std::string& error);

}  // namespace sonora::model
