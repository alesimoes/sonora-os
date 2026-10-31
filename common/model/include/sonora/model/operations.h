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

// Creates a track with one empty playlist, of frames for an audio track and
// of ticks for a MIDI or instrument one. Its mix (input, monitor, faders,
// MIDI routing) is set at the same time and, like any mix, is not undone
// with it. A MIDI track has 0 channels and no input.
struct AddTrack {
    Id track = 0;
    std::string name;
    std::uint16_t channels = 1;
    Id playlist = 0;
    std::string playlist_name;
    TrackMix mix;   // mix.track is ignored; the new track's id is used
    TrackKind kind = TrackKind::Audio;
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

// One pass of a loop take: a new playlist on the take's track holding only
// the pass's region, which plays the take file's span of that pass at A.
struct LoopTakePass {
    Id playlist = 0;
    std::string name;   // "<track name>.T<take>.P<k>"
    Region region;
};

// The most passes one RecordLoopTake places (ADR-0016 O2). Changing it
// changes what a decoder accepts, so it needs a kProtocolVersion bump.
constexpr std::size_t kMaxLoopTakePasses = 99;

// A loop take on one track (ADR-0016 §3), one undo step: every pass, in
// pass order, on a new playlist of its own, and, unless `chosen_region` is
// 0, the chosen pass placed on the active playlist as RecordTake places a
// take, with its crossfades. The active playlist stays active. A loop take
// is an AddSource and one of these per track whatever its pass count. Ids
// rise in this order: each pass's playlist and then its region, then
// chosen_region, then split_right_id.
struct RecordLoopTake {
    Id track = 0;
    Id active_playlist = 0;
    std::vector<LoopTakePass> passes;   // at least one, at most kMaxLoopTakePasses
    std::size_t chosen = 0;             // index into passes
    // The chosen pass's region on the active playlist, as a region of its
    // own with this id; 0 when no pass is complete, which leaves the active
    // playlist as it was (O1).
    Id chosen_region = 0;
    Id split_right_id = 0;
    std::vector<Crossfade> crossfades;  // on the active playlist only
};

// The region operations by id below act on audio and MIDI regions alike.
// Each that carries a position says in `timebase` whether it is frames or
// ticks, and apply() refuses one that does not match the playlist's (A26):
// both are int64_t, and a sender that got it wrong would otherwise
// mis-scale the region by about 25 times without a word.

// Moves a region on its playlist, overwriting what is beneath its new span.
struct MoveRegion {
    Id playlist = 0;
    Id region = 0;
    Frames position = 0;   // in ticks on a MIDI playlist
    Id split_right_id = 0;
    Timebase timebase = Timebase::Frames;
};

enum class Edge : std::uint8_t { Start, End };

// Moves one edge of a region to timeline position `to`, keeping the audio
// or the notes where they are on the timeline. Extending over a neighbour
// overwrites it.
struct TrimRegion {
    Id playlist = 0;
    Id region = 0;
    Edge edge = Edge::End;
    Frames to = 0;         // in ticks on a MIDI playlist
    Timebase timebase = Timebase::Frames;
};

// The left part keeps the region's id; the right part gets `new_right_id`.
// A MIDI region's two parts share its sequence.
struct SplitRegion {
    Id playlist = 0;
    Id region = 0;
    Frames at = 0;         // in ticks on a MIDI playlist
    Id new_right_id = 0;
    Timebase timebase = Timebase::Frames;
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

// Pro Tools' Duplicate Playlist (ADR-0026): a copy of `source`, one of
// `track`'s playlists, as a new playlist of the same track. `playlist` is
// the copy's id and its regions take the ids after it one by one, in
// position order; duplicate_playlist_ids() says how many that is. Sources
// and MIDI sequences are shared. The active playlist does not change: the
// interface sends SetActivePlaylist after it, in the same compound.
struct DuplicatePlaylist {
    Id track = 0;
    Id source = 0;
    Id playlist = 0;
    std::string name;
};

struct SetPlaylistName {
    Id track = 0;
    Id playlist = 0;
    std::string name;
};

// Takes a playlist that is not the active one off its track: Pro Tools'
// Delete Unused Playlists is one of these per playlist, in a compound. Its
// regions' sources stay in the pool (ADR-0009 §3), so undo brings it back
// with its audio.
struct RemovePlaylist {
    Id track = 0;
    Id playlist = 0;
};

// How many ids `op` takes from op.playlist upward on `s`: the copy and each
// of its regions. 0 when op.source is not a playlist of op.track.
std::size_t duplicate_playlist_ids(const Snapshot& s, const DuplicatePlaylist& op);

// Pro Tools' name for a track's next playlist: "<track name>.<nn>", two
// digits or more, the lowest number from 01 that no playlist of the session
// has. Empty when `track` is not a track of `s`.
std::string next_playlist_name(const Snapshot& s, Id track);

// Whether a playlist of `s` other than `except` is named `name`. Pro Tools
// keeps playlist names unique in a session; the model does not require it
// (sessions written before ADR-0026 may repeat one), the interface does.
bool playlist_name_taken(const Snapshot& s, const std::string& name, Id except = 0);

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

// Takes the track out of the session (ADR-0023 §3). Its mix entry and its
// inserts' values stay in MixState, as an undone AddTrack's do, so undo
// brings it back with its faders and plugins as they were; its sources stay
// in the pool.
struct RemoveTrack {
    Id track = 0;
};

// A new track directly below `source`, holding a copy of what it holds
// (ADR-0023 §2), Pro Tools' Duplicate Tracks with its default choices. Every
// object the copy holds gets a new id: `track` is the copy's, and the rest
// follow it one by one, for each playlist copied, in the source's order, the
// playlist and then its regions in position order, then each insert copied,
// in slot order, then each send copied, in slot order. duplicate_track_ids()
// says how many that is. Sources and MIDI sequences are shared, not copied.
// The mix, the inserts' values and the sends' values are copied, solo off.
// The automation lanes are copied unless `automation` is off (ADR-0023,
// update of 2026-10-31); the mode is mix state and is copied either way.
struct DuplicateTrack {
    Id source = 0;
    Id track = 0;
    std::string name;
    bool all_playlists = false;   // false: the active playlist only
    bool inserts = true;          // with their values
    bool sends = true;            // the same buses, with their values
    bool automation = true;       // the lanes
};

// How many ids `op` takes from op.track upward on `s`; 0 when op.source is
// not a track of `s`.
std::size_t duplicate_track_ids(const Snapshot& s, const DuplicateTrack& op);

// Pro Tools' name for a duplicate: "<name>.dup<n>", the lowest n no track of
// `s` has. Empty when `source` is not a track of `s`.
std::string duplicate_track_name(const Snapshot& s, Id source);

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

// --- MIDI (ADR-0014 §1, brain/specs/midi.md §3.1) --------------------------
//
// Every position here is in ticks, resolved by the sender: apply() never
// reads the tempo map to place a region. A take is one compound, labelled
// "Record": AddMidiSource and PlaceMidiRegion (replace), or AddMidiSource,
// EditMidi and TrimRegion (merge).

// Not undoable: the pool is outside undo, as AddSource's is.
struct AddMidiSource {
    MidiSource source;
};

// A region already in the session, whose sequence a new one shares.
struct MidiDataRef {
    Id playlist = 0;
    Id region = 0;
    bool operator==(const MidiDataRef& o) const {
        return playlist == o.playlist && region == o.region;
    }
};

// As PlaceRegion, overwrite included, on a MIDI playlist. The sequence is
// either inline in `region.data` (a recording, an import) or, with
// `data_from`, the one an existing region holds, shared and not copied:
// duplicate, repeat, paste and a move to another track send no notes, and
// every copy charges the undo budget a few words. With `data_from`,
// `region.data` is null or empty.
struct PlaceMidiRegion {
    Id playlist = 0;
    MidiRegion region;
    Id split_right_id = 0;
    std::optional<MidiDataRef> data_from;
};

// A removal names the element by its index in the region's current
// sequence and by the value it expects there. A request resolved against a
// sequence that has changed since (another edit in flight, a merge take)
// is refused by name rather than removing whatever now sits at the index.
struct NoteRemoval {
    std::uint32_t index = 0;
    midi::Note note;
    bool operator==(const NoteRemoval& o) const { return index == o.index && note == o.note; }
};
struct EventRemoval {
    std::uint32_t index = 0;
    MidiEvent event;
    bool operator==(const EventRemoval& o) const { return index == o.index && event == o.event; }
};
struct SysExRemoval {
    std::uint32_t index = 0;
    MidiSysEx sysex;
    bool operator==(const SysExRemoval& o) const { return index == o.index && sysex == o.sysex; }
};

// Every note, event-list, quantize, transpose, velocity and legato edit
// resolves to one of these. Each removal list is strictly increasing by
// index; the additions are values, sorted in. The region gets a sequence of
// its own, so regions that shared the old one (a split, a paste) keep it
// unchanged.
struct EditMidi {
    Id playlist = 0;
    Id region = 0;
    std::vector<NoteRemoval> remove_notes;
    std::vector<midi::Note> add_notes;
    std::vector<EventRemoval> remove_events;
    std::vector<MidiEvent> add_events;
    std::vector<SysExRemoval> remove_sysex;
    std::vector<MidiSysEx> add_sysex;
};

// Mix state, like `input`: saved, not undone (owner question O5).
struct SetMidiRouting {
    Id track = 0;
    MidiRouting routing;
};

// --- sends and aux buses (ADR-0022) ----------------------------------------
//
// Structure (a bus, a send, its slot and its bus) is undoable; values
// (BusMix, SendMix) are mix state, saved and never undone, as a fader is.

// A new aux bus at the end of the display order, with its return's values.
struct AddBus {
    Id bus = 0;
    std::string name;
    BusMix mix;   // mix.bus is ignored; the new bus's id is used
};

// Refused while a send feeds the bus. Its own sends go with it; their
// values, and the bus's, outlive it, so undo brings it back as it was.
struct RemoveBus {
    Id bus = 0;
};

// A send from `strip`, a track or an aux bus, in a free slot, to `bus`.
// Refused if it would close a loop through the buses, or if the strip
// already sends to that bus. Another destination is RemoveSend then
// AddSend in one compound.
struct AddSend {
    Id strip = 0;
    Id send = 0;
    std::uint8_t slot = 0;
    Id bus = 0;
    SendMix mix;   // mix.send is ignored; the new send's id is used
};

struct RemoveSend {
    Id send = 0;
};

// Not undoable.
struct SetSendMix {
    SendMix mix;   // replaces the values of mix.send
};

struct SetBusMix {
    BusMix mix;    // replaces the values of mix.bus
};

// --- output routing (ADR-0024) ----------------------------------------------

// Undoable: routes `strip`, a track with audio or an aux bus, to `output`,
// kMasterOutput or an aux bus. Refused if it would close a loop through the
// buses' sends and outputs.
struct SetOutput {
    Id strip = 0;
    Id output = kMasterOutput;
};

// --- automation (ADR-0027) --------------------------------------------------

// Undoable: replaces the points of `track`'s `param` lane in [from, to)
// with `points`, every one of which lies in that range, sorted, at most two
// at a frame. [0, kMaxInteger) is the whole lane; a lane left with no point
// is removed. Pencil, thin, cut, copy and paste are all this operation.
struct EditAutomation {
    Id track = 0;
    AutomationParam param = AutomationParam::Volume;
    Frames from = 0;
    Frames to = 0;
    std::vector<AutomationPoint> points;
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
                 SetInsertBypass, SetInsertParams, SetInsertState, AddMidiSource,
                 PlaceMidiRegion, EditMidi, SetMidiRouting, RecordLoopTake, AddBus,
                 RemoveBus, AddSend, RemoveSend, SetSendMix, SetBusMix, RemoveTrack,
                 DuplicateTrack, SetOutput, DuplicatePlaylist, SetPlaylistName, RemovePlaylist,
                 EditAutomation>;

// The largest limit measured under the 10 ms control-thread budget on 18
// playlists of 2,000 regions (ADR-0009 note 2026-10-18): members validate
// locally, but each split or removal still shifts the playlist's region
// vector, and 1,024 cut-like members cost 11 to 12 ms. Changing it changes
// what a decoder accepts, so it needs a kProtocolVersion bump. The decoder
// and apply() both refuse more.
constexpr std::size_t kMaxCompoundMembers = 256;

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
                 SetInsertBypass, SetInsertParams, SetInsertState, AddMidiSource,
                 PlaceMidiRegion, EditMidi, SetMidiRouting, RecordLoopTake, AddBus,
                 RemoveBus, AddSend, RemoveSend, SetSendMix, SetBusMix, RemoveTrack,
                 DuplicateTrack, SetOutput, DuplicatePlaylist, SetPlaylistName, RemovePlaylist,
                 EditAutomation, Compound>;

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

// A member of a compound, or a single operation, as a MemberCheck sees it:
// by reference, so a large operation is never copied to be looked at.
class MemberRef {
public:
    explicit MemberRef(const Member& m) : member_(&m) {}
    // `op` is never a Compound: apply() hands over its members instead.
    explicit MemberRef(const Operation& op) : op_(&op) {}

    template <typename T>
    const T* get_if() const {
        return member_ != nullptr ? std::get_if<T>(member_) : std::get_if<T>(op_);
    }
    template <typename T>
    bool holds() const {
        return get_if<T>() != nullptr;
    }

private:
    const Member* member_ = nullptr;
    const Operation* op_ = nullptr;
};

// An engine's own refusal of a member, asked before the member is applied,
// with the state the members before it made: whether a plugin can be
// hosted, say, which depends on a track an earlier member added. Empty
// accepts. Keeping a copy of `at` is safe: the copy is one more owner, and
// apply() then edits nothing in place. Keeping a reference or pointer to
// it, or to anything in it, past the call is not: apply() edits in place
// what only the state it is building holds.
using MemberCheck = std::function<std::string(const Snapshot& at, MemberRef member)>;

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

// --- track order, for an engine that maps tracks by position (ADR-0023) ----

std::vector<Id> track_order(const EditState& e);

// The display order `member` leaves on `at`, without applying it: true, with
// `order`, for AddTrack, RemoveTrack, ReorderTracks and DuplicateTrack that
// apply() would accept as far as the tracks go; false for anything else.
bool track_order_after(const Snapshot& at, MemberRef member, std::vector<Id>& order);

// Empty, or why `after` may not replace the order of `before` while the
// tracks at the positions `pinned` marks are being written: each must stay
// where it is. `doing` and `remedy` complete the words: "\"Vox\" <doing>:
// <remedy> before deleting it".
std::string pinned_track_refusal(const EditState& before, const std::vector<Id>& after,
                                 const std::vector<bool>& pinned, const std::string& doing,
                                 const std::string& remedy);

// Why no change of track order or set goes through while a take is about to
// start, or while take `take` is closing: its files are placed on the
// tracks at their rows when they are in (ADR-0023 §5). The same words from
// every engine.
std::string take_starting_track_refusal();
std::string take_closing_track_refusal(int take);

// --- the active playlist during a take (ADR-0026 §4) ------------------------
//
// A take goes onto the active playlist of its track as that playlist is
// when the take's files are in (closed_take.cpp), so the playlist a take
// records onto may not change from Record until then.

// The tracks, by id, whose active playlist differs between `before` and
// `after`; a track only one of them holds is not one.
std::vector<Id> active_playlist_changes(const EditState& before, const EditState& after);

// The same for `member` applied to `at`, without applying it: the track of
// a SetActivePlaylist that names a playlist other than its active one.
std::vector<Id> active_playlist_changes(const Snapshot& at, MemberRef member);

// Empty, or why the tracks `changed` may not choose another playlist while
// the tracks at the rows `pinned` marks record: "\"Vox\" is recording: stop
// recording before choosing another playlist".
std::string pinned_playlist_refusal(const EditState& before, const std::vector<Id>& changed,
                                    const std::vector<bool>& pinned);

// While a take is about to start, or while take `take` is closing, no track
// chooses another playlist. The same words from every engine.
std::string take_starting_playlist_refusal();
std::string take_closing_playlist_refusal(int take);

}  // namespace sonora::model
