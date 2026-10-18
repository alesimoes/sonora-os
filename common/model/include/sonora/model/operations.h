// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operations: every change to the document, as a plain value that carries
// every id and number its result depends on (timeline-model.md,
// "Operations"). apply() is therefore a pure function: it allocates no id,
// reads no clock and touches no file, so the engine and a UI replica that
// apply the same operations reach equal snapshots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

// --- inserts (ADR-0012 §6.3) ------------------------------------------------
//
// Structure (which plugin, which slot, active) is undoable: owner decision
// O1's default. Values (bypass, parameters, state) are mix state either
// way, saved and never undone, as a fader is.
//
// Flipping O1 is not small. is_undoable() is five lines, but
// TrackEdit::inserts moves into MixState, and with it apply.cpp's
// locate_insert and edit_insert, same_document(), the session reader, and
// the undo-window codec, which serialises inserts inside each edit track
// (kWindowFormat 2) and so changes format. Only the operation codec and the
// session file stay as they are.

// A new insert in a free slot of `track`, active, with these values.
struct AddInsert {
    Id track = 0;
    Id insert = 0;
    std::uint8_t slot = 0;
    PluginRef plugin;
    std::vector<ParamValue> params;
    std::optional<StateRef> state;
};

// Its values outlive it in MixState, so undoing the removal brings the
// plugin back as it was.
struct RemoveInsert {
    Id insert = 0;
};

// Between slots of one track or onto another track; refused onto a slot
// another insert holds, and onto a track of another channel count.
struct MoveInsert {
    Id insert = 0;
    Id to_track = 0;
    std::uint8_t to_slot = 0;
};

// Pro Tools' active and inactive: in the chain or out of it.
struct SetInsertActive {
    Id insert = 0;
    bool active = true;
};

// Another plugin in the same slot: the insert is removed and `replacement`
// takes its slot and its active state, with these values, as one undo
// step. A new id, because the old one's values belong to the old plugin and
// undo brings them back with it.
struct ReplaceInsert {
    Id insert = 0;
    Id replacement = 0;
    PluginRef plugin;
    std::vector<ParamValue> params;
    std::optional<StateRef> state;
};

struct SetInsertBypass {
    Id insert = 0;
    bool bypass = false;
};

// Every value the insert holds, replacing the list: the end of a gesture,
// and every insert's values at save.
struct SetInsertParams {
    Id insert = 0;
    std::vector<ParamValue> params;
};

// At save, after the engine has had the plugin's state written to its file.
struct SetInsertState {
    Id insert = 0;
    std::optional<StateRef> state;
};

// Every operation but Compound: what a compound holds. The type, not a
// runtime check, keeps a compound out of a compound.
using Member =
    std::variant<AddSource, SetMix, SetMonitorBus, SetSettings, AddTrack,
                 PlaceRegion, RecordTake, MoveRegion, TrimRegion, SplitRegion,
                 RemoveRegion, SetFade, SetCrossfade, RemoveCrossfade,
                 AddPlaylist, SetActivePlaylist, AddMarker, MoveMarker,
                 RemoveMarker, SetTrackName, ReorderTracks, AddInsert,
                 RemoveInsert, MoveInsert, SetInsertActive, ReplaceInsert,
                 SetInsertBypass, SetInsertParams, SetInsertState>;

// ADR-0009 update 2026-10-14: about 200 members per track on 18 tracks.
// The decoder and apply() both refuse more.
constexpr std::size_t kMaxCompoundMembers = 4096;

// Several operations as one: each member applied to what the members before
// it made, all refused if one is, one revision and one update. It makes one
// edit-undo step if any member is edit-undoable, and none otherwise.
// Whoever builds one gives its members strictly increasing ids from
// next_id, reserving every conditional id (split_right_id) whether or not
// it is used.
struct Compound {
    std::vector<Member> ops;
    // What the Edit menu and the undo history show ("Undo Paste"). Display
    // only: apply() ignores it. Empty means none.
    std::string label;
};

// The same alternatives as Member, at the same indices, then Compound.
using Operation =
    std::variant<AddSource, SetMix, SetMonitorBus, SetSettings, AddTrack,
                 PlaceRegion, RecordTake, MoveRegion, TrimRegion, SplitRegion,
                 RemoveRegion, SetFade, SetCrossfade, RemoveCrossfade,
                 AddPlaylist, SetActivePlaylist, AddMarker, MoveMarker,
                 RemoveMarker, SetTrackName, ReorderTracks, AddInsert,
                 RemoveInsert, MoveInsert, SetInsertActive, ReplaceInsert,
                 SetInsertBypass, SetInsertParams, SetInsertState, Compound>;

static_assert(std::variant_size_v<Operation> == std::variant_size_v<Member> + 1,
              "Operation is Member's alternatives and Compound");

Operation to_operation(const Member& m);
// False, leaving `out` alone, for a Compound.
bool to_member(Operation op, Member& out);

// Whether applying the operation makes an edit-undo step. The ones that do
// not leave Snapshot::edit untouched. A compound makes one if any member
// would. Mix and plug-in values make none: their own undo history is
// BACKLOG Track 2 item 6.
bool is_undoable(const Operation& op);
bool member_is_undoable(const Member& m);

// An engine's own refusal of a member, asked before the member is applied,
// with the state the members before it made: whether a plugin can be
// hosted, say, which depends on a track an earlier member added. Empty
// accepts. It must not keep `at`: apply() edits in place what only the
// state it is building holds.
using MemberCheck = std::function<std::string(const Snapshot& at, const Member& member)>;

// The pure function. On success `after` is the new snapshot, sharing every
// unchanged part with `before`, with revision + 1. On failure `after` is
// untouched and `error` says why; `before` is never modified either way.
// A result always satisfies check_invariants().
//
// A compound fails as a whole, naming the member counted from 1:
// "operation 4 of 36 (RecordTake): <the member's reason>". `check`, if set,
// is asked before each member (or before a single operation, whose refusal
// is then `error` as it stands).
bool apply(const Snapshot& before, const Operation& op, Snapshot& after,
           std::string& error, const MemberCheck& check = {});

}  // namespace sonora::model
