// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operations: every change to the document, as a plain value that carries
// every id and number its result depends on (timeline-model.md,
// "Operations"). apply() is therefore a pure function: it allocates no id,
// reads no clock and touches no file, so the engine and a UI replica that
// apply the same operations reach equal snapshots.
#pragma once

#include <string>
#include <variant>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model {

// --- not undoable -----------------------------------------------------------

// Sources are a pool outside undo: undoing a take removes its region, never
// its audio.
struct AddSource {
    Source source;
};

// A fader move is not an edit (timeline-model.md, "What undo covers").
struct SetMix {
    TrackMix mix;   // replaces the mix of mix.track
};

struct SetMonitorBus {
    MonitorBus bus;
};

// Timecode, tempo, loop, punch, name. The sample rate cannot change: every
// source was recorded at it.
struct SetSettings {
    SessionSettings settings;
};

// --- undoable ---------------------------------------------------------------

// Creates a track with one empty playlist. Its mix (input, monitor, faders)
// is set at the same time and, like any mix, is not undone with it.
struct AddTrack {
    Id track = 0;
    std::string name;
    std::uint16_t channels = 1;
    Id playlist = 0;
    std::string playlist_name;
    TrackMix mix;   // mix.track is ignored; the new track's id is used
};

// Places `region` over [position, end) on `playlist`, overwriting what is
// beneath (see Overwrite in timeline-model.md). A region beneath that spans
// both ends is split, and its right part gets `split_right_id`; the
// operation is refused if that split is needed and no id was given.
struct PlaceRegion {
    Id playlist = 0;
    Region region;
    Id split_right_id = 0;
};

// A closed take: PlaceRegion on the track's playlist, plus the crossfades
// the punch asked for, all one undo step. Each crossfade must join the new
// region to a neighbour it butts after the overwrite; one without handle
// material is refused, not shortened.
struct RecordTake {
    Id track = 0;
    Id playlist = 0;
    Region region;
    Id split_right_id = 0;
    std::vector<Crossfade> crossfades;
};

// Moves a region on its playlist, overwriting what is beneath its new span.
struct MoveRegion {
    Id playlist = 0;
    Id region = 0;
    Frames position = 0;
    Id split_right_id = 0;
};

enum class Edge : std::uint8_t { Start, End };

// Moves one edge of a region to timeline frame `to`, keeping the audio where
// it is on the timeline. Extending over a neighbour overwrites it.
struct TrimRegion {
    Id playlist = 0;
    Id region = 0;
    Edge edge = Edge::End;
    Frames to = 0;
};

// The left part keeps the region's id; the right part gets `new_right_id`.
struct SplitRegion {
    Id playlist = 0;
    Id region = 0;
    Frames at = 0;
    Id new_right_id = 0;
};

struct RemoveRegion {
    Id playlist = 0;
    Id region = 0;
};

enum class FadeEnd : std::uint8_t { In, Out };

struct SetFade {
    Id playlist = 0;
    Id region = 0;
    FadeEnd end = FadeEnd::In;
    Fade fade;
};

// Adds the crossfade, or replaces the one between the same two regions.
struct SetCrossfade {
    Id playlist = 0;
    Crossfade crossfade;
};

struct RemoveCrossfade {
    Id playlist = 0;
    Id left = 0;
    Id right = 0;
};

struct AddPlaylist {
    Id track = 0;
    Id playlist = 0;
    std::string name;
};

struct SetActivePlaylist {
    Id track = 0;
    Id playlist = 0;
};

struct AddMarker {
    Marker marker;
};

struct MoveMarker {
    Id marker = 0;
    Frames position = 0;
};

struct RemoveMarker {
    Id marker = 0;
};

struct SetTrackName {
    Id track = 0;
    std::string name;
};

// A permutation of every track id, in the new display order.
struct ReorderTracks {
    std::vector<Id> order;
};

using Operation =
    std::variant<AddSource, SetMix, SetMonitorBus, SetSettings, AddTrack,
                 PlaceRegion, RecordTake, MoveRegion, TrimRegion, SplitRegion,
                 RemoveRegion, SetFade, SetCrossfade, RemoveCrossfade,
                 AddPlaylist, SetActivePlaylist, AddMarker, MoveMarker,
                 RemoveMarker, SetTrackName, ReorderTracks>;

// Whether applying the operation makes an undo step. The non-undoable ones
// leave Snapshot::edit untouched.
bool is_undoable(const Operation& op);

// The pure function. On success `after` is the new snapshot, sharing every
// unchanged part with `before`, with revision + 1. On failure `after` is
// untouched and `error` says why; `before` is never modified either way.
// A result always satisfies check_invariants().
bool apply(const Snapshot& before, const Operation& op, Snapshot& after,
           std::string& error);

}  // namespace sonora::model
