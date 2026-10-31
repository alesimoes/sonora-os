// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages between an interface and the engine (ADR-0007 §4.1).
//
// Two kinds of traffic, kept apart on purpose. Continuous values (meters,
// the playhead, arm state, counters) are state: a client pulls them from
// the shared state block at its own rate (state_block.h, state_view.h).
// Discrete facts (a command's result, a take that failed, the server gone,
// a model operation) are messages: they arrive in order through poll().
//
// Every message crosses the wire codec (wire.h), in process as well as
// across one, so M7 swaps the transport and nothing else.
//
// Never on the audio thread (ADR-0007 §2): these types hold std::string and
// std::vector. RT code may not even copy them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/command.h"

namespace sonora::api {

// Bump on any change to a message's encoding, or a new operation kind the
// engine may send: a replica cannot apply what it cannot decode, and would
// resync for ever. A client and an engine that disagree refuse each other
// at connect, by name, with both numbers.
// 2 (M7): ModelAction::Load.
// 3 (ADR-0012 §8): PluginParameterChange, PluginCatalog,
//   EventKind::PluginFault, EngineEvent::insert,
//   ModelAction::FlushPluginState; insert operations ride ModelRequest as
//   before.
// 4 (ADR-0009 update 2026-10-14): Compound, in common/model's op codec;
//   ModelUpdate::label, the step an Undo or Redo moved over; the undo
//   window's format 3, with each step's label.
// 5 (ADR-0009 note 2026-10-18, BACKLOG Track 2 item 8): a Compound may hold
//   up to model::kMaxCompoundMembers = 256 members, not 64; a version-4
//   replica would refuse to decode the larger ones.
// 6 (BACKLOG Track 2 item 5): a track's mix carries solo_safe, in SetMix,
//   AddTrack and the undo window (format 4); a version-5 peer's decoder
//   refuses the key, and this one requires it.
// 7 (ADR-0014 M-1): MIDI in the model. New operation kinds AddMidiSource,
//   PlaceMidiRegion, EditMidi and SetMidiRouting; AddTrack carries `kind`;
//   MoveRegion, TrimRegion and SplitRegion carry `timebase`; a track's mix
//   carries its MIDI routing; the undo window's format 5.
// 8 (ADR-0015 M-1c): Load carries a session bundle, the session file with
//   the MIDI sequence files it names (session format 6); the undo window's
//   format 6, where a sequence whose file was missing at open carries its
//   reference.
// 9 (ADR-0016 §3): loop record. A new operation kind, RecordLoopTake, which
//   a version-8 peer's decoder refuses.
// 10 (ADR-0015 M-1c PR 2; 9 is loop record's): a Snapshot travels as
//   ModelUpdateKind::SnapshotHeader, SnapshotPart and Snapshot, the window's
//   chunk table in parts of at most wire::kMaxPartBytes (undo window format
//   7, sequences as spines of chunks); a Load as ModelAction::LoadPart
//   requests, slices of the same chunk table, ahead of a Load that carries
//   their index by sequence and the bare session file.
// 11 (ADR-0015 update of 2026-10-25, M-1c PR 3): ModelAction::Save; the
//   engine writes the session folder.
// 12 (ADR-0016 pre-roll and post-roll): Hello::capabilities, appended, so a
//   version-11 peer's decoder refuses this Hello and this one refuses its.
// 13 (engine.record.orphan_offer): EventKind::RecoveryOffer, which a
//   version-12 peer's decoder refuses.
// 14 (ADR-0017 Y2): PluginInfo::bridge, after has_editor, so the operator
//   sees which plugins run under Wine; a version-13 peer's decoder refuses
//   this catalog, and this one refuses its.
// 15 (ADR-0020): SetSettings carries transport.count_off_bars (session
//   format 8); a version-14 peer's op decoder refuses the key, and this one
//   requires it.
// 16 (ADR-0018 §2, §4): ModelAction::Load's `op` is wire::encode_load(),
//   the session file's path ahead of the bytes; EventKind::SessionFolder,
//   and EngineEvent::flags after `text`.
// 17 (transport.toggle_loop): SetSettings carries transport.loop_playback
//   (session format 9), refused and required as 15's bars are;
//   transport.toggle_loop takes the selection as an argument.
// 20 (ADR-0018 §3, untitled sessions; 19 was never used: the track input
//   monitor, #148, merged at 17 without a bump, and 18, held for loop
//   record, was renumbered when it landed later): Load's path may name a
//   folder with no session file, an unsaved recording, entered untitled; a
//   Load with an empty path is a new session in a fresh Untitled folder
//   when the engine has a sessions root; the first Save of an untitled
//   session renames its folder to the Save's, refused while a take is open
//   or closing. A version-17 engine refuses the folder as "no session file".
// 22 (ADR-0019, update of 2026-10-26; 21 was never used: loop record took
//   23 when it merged after this): EventKind::Latency, whose flags are a
//   LatencySource and a LatencyStatus; a version-20 peer's decoder refuses
//   the kind.
// 23 (transport.toggle_loop_record): SetSettings carries
//   transport.loop_record (session format 10), refused and required as 17's
//   loop_playback is; transport.toggle_loop_record, transport.toggle_cycle
//   (the selection as an optional argument, the range when none is set) and
//   transport.cycle_selection (the selection, required).
// 25 (ADR-0016, update "transport settings at the authority"; 24 is held
//   for the fades PR in flight): transport.punch_selection (the selection,
//   required), transport.clear_punch, transport.set_pre_roll and
//   transport.set_post_roll (frames) are engine commands, and the engine
//   refuses a punch range change, by command or SetSettings, while a take
//   is open or requested. A version-23 engine left them to the interface's
//   whole-settings SetSettings.
// 27 (ADR-0022 sends and aux buses; 26 is held for a PR in flight): the
//   operation codec has AddBus, RemoveBus, AddSend, RemoveSend, SetSendMix
//   and SetBusMix, and a Snapshot's undo window is window format 8, with aux
//   buses and sends (session format 11). A version-25 peer refuses all of
//   them.
// 28 (ADR-0023; 26 was never used, numbered while 25 was in flight, and 24
//   is still held for fades): new operation kinds RemoveTrack and
//   DuplicateTrack, which a version-27 peer's decoder refuses.
// 29 (daw.model.session_mgmt, ADR-0018 update of 2026-10-27):
//   ModelAction::SaveAs, Save As and Save Copy In, which a version-28 peer's
//   decoder refuses as an unknown action.
// 30 (ADR-0024; 29 is PR #164's): the operation codec has SetOutput, a
//   BusMix carries its solo, and a Snapshot's undo window is window format
//   9, with each track's and bus's output (session format 12). A version-28
//   peer refuses all of them.
// 31 (ADR-0026 playlists; follows 30): new operation kinds DuplicatePlaylist,
//   SetPlaylistName and RemovePlaylist, which a version-28 peer's decoder
//   refuses.
// 32 (ADR-0027; follows 31): the operation codec has EditAutomation, a
//   TrackMix carries its automation mode, and a Snapshot's undo window is
//   window format 10, with each track's automation lanes (session format
//   13). A version-31 peer refuses all of them.
// 33 (ADR-0027 §3, the write slice; follows 32): ParameterChange carries a
//   gesture (touch, move, release) and may drive ParamId::Pan; a version-32
//   peer's decoder refuses both, as left-over bytes or an unknown parameter.
// 34 (ADR-0022 update of 2026-10-31; follows 33): ParamId::SendLevel and
//   ParamId::AuxGain, and ParameterChange's `id`; a version-33 peer refuses
//   both, as an unknown parameter or left-over bytes.
constexpr uint32_t kProtocolVersion = 34;

// Hello::capabilities: transport.play and transport.toggle_play take an
// "A-B" frame range as their argument and play it with pre- and post-roll.
// An engine without it ignores the argument and plays from the playhead with
// no end, so an interface sends the range only to an engine announcing this.
inline constexpr const char* kCapabilityPlayRange = "transport.play.range";

// --- client -> engine -------------------------------------------------------

// A named, key-mappable, discrete command. `seq` is assigned by
// EngineControl::send(), which returns it; whatever the caller put there is
// overwritten.
struct CommandRequest {
    uint64_t seq = 0;
    std::string command;   // an id from command_catalog(), Engine domain
    int32_t target = -1;   // track index; negative means none given
    std::string argument;
};

// Continuous values: fader drags, surface faders. Append-only enum.
enum class ParamId : uint32_t {
    Gain = 0,   // linear, 0 .. 4 (+12 dB)
    Pan = 1,    // -1 (left) .. +1 (right)
    // ADR-0022 update of 2026-10-31: addressed by ParameterChange::id, not
    // by track. Linear, 0 .. 4, as Gain.
    SendLevel = 2,
    AuxGain = 3,
};

// Where a change sits in the operator's gesture (ADR-0027 §3). In Touch,
// Latch and Write a parameter plays its live value instead of its lane from
// Touch on, and in Touch returns to the lane from Release. Move is every
// step between, and all a surface without touch sensing sends.
enum class Gesture : uint32_t { Move = 0, Touch = 1, Release = 2 };

struct ParameterChange {
    uint64_t seq = 0;
    uint32_t track = 0;
    ParamId param = ParamId::Gain;
    float value = 0.0f;
    Gesture gesture = Gesture::Move;
    // The send's model id for SendLevel, the aux bus's for AuxGain; `track`
    // is ignored for them. An id, because a track's send changes strip
    // index when the tracks are reordered.
    uint64_t id = 0;
};

// A value moved in the plugin's own editor (ADR-0012 §8) comes from the
// host, not from a client, so no client sends its SetInsertParams. The
// engine records it itself: at the end of each ParamTouched gesture, and
// on EditorClosed for whatever moved without a gesture end (an LV2 UI that
// never says one), it applies SetInsertParams with the insert's whole
// value list, the moved keys set, and every replica gets it as a
// ModelUpdate like any other. So the generic panel, compare and the saved
// values follow the editor, at most one gesture behind. Nothing is sent
// while a gesture runs.
//
// A plugin parameter moving: a knob drag or a surface encoder (ADR-0012 §8).
// ParameterChange stays track-only. The engine maps it to the insert's
// chain and hands it to the audio thread; the end of the gesture is saved
// with the model's SetInsertParams, as a fader's is with SetMix.
//
// Addressed by insert id and stable key, never by the catalog's dense
// index: a rescan may reorder a plugin's parameters while a drag is in
// flight, and an index from the old catalog would then move another
// parameter. The engine resolves the key against the running instance's
// own table, built by the scan that instantiated it, so the change reaches
// the parameter the operator touched or is refused. Protocol version 3.
struct PluginParameterChange {
    uint64_t seq = 0;
    uint64_t insert = 0;   // the insert's model id
    // PluginParam::key: the LV2 port symbol or the VST3 ParamID in decimal,
    // as the session file stores it (ADR-0012 §6.1).
    std::string key;
    // Format-native (ADR-0012 §5.2): the LV2 port value, or the VST3
    // normalised value. The engine refuses one outside the parameter's range.
    float value = 0.0f;
};

// The model path (ADR-0009 §2). The engine owns the document; a client
// submits a fully resolved operation and learns the outcome twice: a
// CommandReply for its own request, and a ModelUpdate that every replica
// receives, its own included. A client never applies its own operation.
// Append-only enum.
enum class ModelAction : uint32_t {
    Apply = 0,   // `op` is one operation, encoded by common/model's op codec
    Undo = 1,
    Redo = 2,
    Resync = 3,  // send me the whole document; I lost track of the revisions
    // Open another session: `op` is a session bundle, the session file with
    // the MIDI sequence files it names, or a bare session file (common/
    // model's session_file.h, decode_session_bundle). The engine's document
    // becomes it, with a new undo history, and every replica gets a
    // Snapshot. Bytes rather than a path, so the engine needs no view of the
    // interface's filesystem and File > Open's checks stay where the dialog
    // is. Protocol version 2; a bundle since 8; since 10, or after LoadPart
    // requests, an index of the parts' chunks by sequence and the bare
    // session file (common/model's encode_load_parts). Refused, the session
    // kept, when the new document's Snapshot could not be sent.
    //
    // Since 16 `op` is wire::encode_load(): the session file's absolute path
    // in the engine's filesystem (the control socket is local, as for Save),
    // then those bytes. The engine reads the session from the bytes, not
    // the file; the path says which folder the session owns (ADR-0018 §2).
    // It checks that the path is a regular file and enters its session
    // folder, "Session File Backups/" resolving to the folder above, before
    // the document swaps: lock, recovery, medium. A refusal there keeps the
    // old session, its document and its folder. Refused while a take is
    // open or closing: the take belongs to the session it was started in.
    // Since 20 (ADR-0018 §3) the path may name a folder holding no session
    // file, an unsaved recording, which is entered untitled; one holding a
    // session file is refused, naming it. An empty path is a new session:
    // a fresh Untitled folder under the engine's sessions root (the active
    // one if it is untitled and holds nothing), or, with no root (--dir
    // runs, tests), the active folder. And a Save of an untitled session
    // into a folder that does not exist, beside it on its filesystem,
    // renames its folder there first, refused during a take.
    Load = 4,
    // Bring the document's plugin values up to date with the running
    // plugins (ADR-0012 §8): before File > Save, and before an insert is
    // copied or compared. `op` is empty for every insert, or one insert's id
    // in decimal. The engine has each running plugin's state written to its
    // file and applies SetInsertState, and SetInsertParams for values moved
    // in the plugin's own editor and not yet recorded; every replica gets
    // those as ModelUpdates, and the reply comes after the last of them, so
    // once it is Ok the replica holds what a session file must. Failed names
    // each insert whose state could not be had (its host down, GetState
    // timed out); the others' updates still went out. An insert that is not
    // running already has its last state in the document. Protocol 3.
    FlushPluginState = 5,
    // One part of a Load in parts (ADR-0015 §4, protocol 10): a slice of the
    // session's MIDI chunk table in the Snapshot's own part form, whole
    // chunks, at most wire::kMaxPartBytes (common/model's encode_load_parts),
    // so a sequence of any size crosses. The engine decodes and checks each
    // part as it arrives, and replies to each; the Load that follows says
    // which chunks make each sequence. A refused part abandons the Load:
    // the parts before it are dropped and the Load is refused with its
    // reason. The parts are this client's alone until its next Load.
    LoadPart = 6,
    // File > Save (ADR-0015 update of 2026-10-25, protocol 11): `op` is
    // wire::encode_save(), the session file's absolute path in the engine's
    // filesystem (the control socket is local) and the canonical bytes the
    // client serialised from the document it shows. The engine's saver
    // thread writes each MIDI sequence file those bytes name, from the
    // engine's own document, then the session file, atomically, and deletes
    // sequence files no kept session file names. The reply comes when the
    // save is done: Ok with an empty message when it is durable, Ok with a
    // caution when the file was written but not made durable, Failed with
    // the reason, the previous file as it was, otherwise. A sequence the
    // bytes name that the engine's document does not hold (an edit it
    // refused) fails the save. While a take is open the save waits for the
    // disk thread's next checkpoint, and every client is told so.
    //
    // Since 29 each save keeps the bytes it replaced as a rotated backup in
    // "Session File Backups/", the newest 20 by default, and the engine
    // autosaves its own document beside them (session-format.md, "Saving"
    // step 5, "Autosave"); neither changes this request.
    Save = 7,
    // File > Save As and File > Save Copy In (protocol 29; ADR-0018 §5 and
    // its update of 2026-10-27): `op` is wire::encode_save_as(), the new
    // session file's absolute path, the kSaveAs* flags and the bytes, as for
    // Save. The engine makes the file's folder, which must not exist or be
    // empty, a self-contained session: every file of the active folder's
    // "Audio Files/" and "MIDI Files/" linked or copied there, the plugin
    // states and sequence files the bytes name, and the session file last.
    // The active folder is only read. Then, unless kSaveAsCopy, the engine
    // enters the new folder, announcing it in a SessionFolder event before
    // the reply: takes from then on go there. Ok with a caution when the
    // copy is written but the session could not move (the reply says why and
    // the SessionFolder event still names the old folder). Refused while a
    // take is open, closing or about to start, for a folder inside the
    // active one or holding it, and for kSaveAsNoAudio without kSaveAsCopy:
    // a session the operator goes on with never points outside its folder.
    SaveAs = 8,
};

// ModelAction::SaveAs's flags.
// Save Copy In: the session stays in its folder; the copy is left as made.
inline constexpr uint32_t kSaveAsCopy = 1u << 0;
// Save Copy In without "All audio files": the session file, plugin states
// and sequence files only; the copy's audio is offline until it is copied
// in by hand. Only with kSaveAsCopy.
inline constexpr uint32_t kSaveAsNoAudio = 1u << 1;
inline constexpr uint32_t kSaveAsFlags = kSaveAsCopy | kSaveAsNoAudio;

struct ModelRequest {
    uint64_t seq = 0;
    ModelAction action = ModelAction::Apply;
    // Opaque here: contracts/api may not depend on common/model (ADR-0007
    // §1), so the operation crosses as bytes and common/model decodes them.
    std::string op;
};

// --- engine -> client -------------------------------------------------------

// First message after connect. A version mismatch is refused, loudly.
struct Hello {
    uint32_t protocol_version = 0;
    uint32_t state_layout_version = 0;
    uint32_t sample_rate = 0;
    uint32_t buffer_frames = 0;   // 0 until the backend's first period
    uint32_t track_count = 0;
    std::vector<std::string> track_names;   // track_count entries
    // Engine-domain commands with a handler. Together with the client's own
    // registry this is ensure_essentials()'s `implemented` (ADR-0007 §5).
    std::vector<std::string> implemented_commands;
    // What the engine does beyond the command ids, named by the kCapability
    // constants above; an id this build does not know is ignored.
    std::vector<std::string> capabilities;
};

// The answer to one CommandRequest, ParameterChange or ModelRequest.
struct CommandReply {
    uint64_t seq = 0;
    DispatchResult result = DispatchResult::Ok;
    std::string message;   // the reason when result is not Ok; may be empty
};

// Append-only enum.
enum class EventKind : uint32_t {
    // A line the interface keeps on screen for the session ("recording from
    // onboard audio: no USB interface found").
    Notice = 0,
    // Why no input is connected; empty text means they are connected again.
    // The interface shows the latest one until it is cleared.
    InputsMissing = 1,
    // The open take stopped writing (full disk, 4 GB limit); `track` is -1
    // and `frame` the take number. Empty text clears it: the next take
    // records cleanly.
    TakeFailed = 2,
    // The backend stopped calling the engine (server gone, rate or quantum
    // change, device gone). The engine closes the take; the interface says
    // why and leaves.
    Fatal = 3,
    // An insert is down: its host crashed, was removed for missed blocks,
    // is restarting, refused the plugin, produced non-finite samples; the
    // plugin is missing; or plugins cannot be hosted at all (ADR-0012 §8).
    // `insert` is the insert's model id, which a MoveInsert or a
    // ReorderTracks in flight does not change; `track` is -1, and the
    // interface finds the slot in its replica. The text names the plugin,
    // its track and its slot, for a log. One fault stands per insert, the
    // latest replacing the one before; empty text clears it (the insert
    // runs again, or is gone: removed, undone, inactive, another session
    // opened). A client that attaches later is sent every fault that still
    // stands, and no other. Protocol 3.
    PluginFault = 4,
    // Takes on disk the open session does not name: one an engine died
    // recording, or one closed after the last save (session-format.md,
    // "Crash recovery"). `text` says where each would go, one line per
    // take; `frame` is how many there are. The interface offers them, and
    // nothing is placed until it sends session.place_recovered (or
    // session.leave_recovered leaves them in Audio Files and records them
    // in the document's settings.left_takes, so they are not offered
    // again). Only takes of the open session are offered: by the session
    // id in their Description. One offer stands at a time, replaced at
    // every session open; empty text withdraws it. A client that attaches
    // later is sent the one that stands. Protocol 13.
    RecoveryOffer = 5,
    // Where the open session lives (ADR-0018 §4): `text` is the active
    // session folder's absolute path in the engine's filesystem, `flags`
    // kFolderUntitled and kFolderReadOnly, `frame` the medium's free bytes
    // when the folder was entered (0: not known). Its takes, wrap lists and
    // plugin states are written there, so a Save of this session belongs
    // there too. One stands from the engine's start, replaced at every
    // folder change; a client that attaches later is sent it. Protocol 16.
    SessionFolder = 6,
    // The round trip L takes are compensated by, and the loopback probe
    // (ADR-0019, update of 2026-10-26): `frame` is L in frames, `flags`
    // latency_flags(), its source and the probe's status, `insert` how many
    // outputs the probe may play on, `text` the probe's own words with its
    // figures (empty before any measurement). One stands from the engine's
    // start, replaced at every change and at every ping while measuring; a
    // client that attaches later is sent it. Protocol 22.
    Latency = 7,
};

// EngineEvent::flags of a SessionFolder.
inline constexpr uint32_t kFolderUntitled = 1u << 0;   // the engine made it ("Untitled <n>")
inline constexpr uint32_t kFolderReadOnly = 1u << 1;   // the medium refuses writes

// Where L came from. Append-only.
enum class LatencySource : uint32_t {
    None = 0,      // nothing measured, given or reported: L is 0
    Measured = 1,  // the loopback probe, now or kept for this machine
    Manual = 2,    // latency.set or --input-latency
    Backend = 3,   // AudioBackend::reported_latency(), the fallback
};
// What the probe is doing, or how its last measurement ended: Measured,
// or one of LatencyProbe's five failures. Append-only.
enum class LatencyStatus : uint32_t {
    Idle = 0,      // nothing measured since the engine started
    Measuring = 1,
    Measured = 2,
    NoSignal = 3,
    Unstable = 4,
    Discontinuity = 5,
    NoPort = 6,
    Cancelled = 7,
};
inline constexpr uint32_t kLastLatencySource = static_cast<uint32_t>(LatencySource::Backend);
inline constexpr uint32_t kLastLatencyStatus = static_cast<uint32_t>(LatencyStatus::Cancelled);
// A Latency event's flags: the source in the low byte, the status above it.
inline constexpr uint32_t latency_flags(LatencySource source, LatencyStatus status) {
    return static_cast<uint32_t>(source) | static_cast<uint32_t>(status) << 8;
}
inline constexpr LatencySource latency_source(uint32_t flags) {
    return static_cast<LatencySource>(flags & 0xFFu);
}
inline constexpr LatencyStatus latency_status(uint32_t flags) {
    return static_cast<LatencyStatus>((flags >> 8) & 0xFFu);
}

struct EngineEvent {
    EventKind kind = EventKind::Notice;
    int32_t track = -1;
    // TakeFailed: the take number. RecoveryOffer: how many takes.
    // SessionFolder: free bytes. Latency: L in frames. 0 otherwise.
    uint64_t frame = 0;
    // PluginFault: the insert's model id. Latency: the outputs the probe
    // may use. 0 otherwise. Protocol 3.
    uint64_t insert = 0;
    std::string text;
    // SessionFolder: kFolder* bits. Latency: latency_flags(). 0 otherwise.
    // Protocol 16.
    uint32_t flags = 0;
};

// One change to the document, sent to every replica in revision order.
// Append-only enum.
enum class ModelUpdateKind : uint32_t {
    Op = 0,        // apply `bytes` (one encoded operation)
    Undo = 1,      // move the replica's own pointer back one step
    Redo = 2,      // and forward
    // Replace the replica with `bytes`: the document and its whole undo
    // window (common/model's encode_window), so undo and redo after it
    // apply on the replica as on the engine. Sent after Hello, on Resync and
    // after a Load, after its header and parts; one whose chunk table is
    // empty (no MIDI) comes alone.
    Snapshot = 3,
    // A Snapshot begins (protocol 10): `part_count` SnapshotParts follow,
    // `part_bytes` together, then the Snapshot. Nothing else reaches this
    // client in between. A replica refuses a header over its own cap.
    SnapshotHeader = 4,
    // One part of the window's chunk table, whole chunks, at most
    // wire::kMaxPartBytes; decoded and checked on its own as it arrives.
    SnapshotPart = 5,
};

struct ModelUpdate {
    ModelUpdateKind kind = ModelUpdateKind::Op;
    // The revision the document has after this update. A replica that
    // expected another one has missed something and asks to Resync.
    uint64_t revision = 0;
    // The oldest revision undo can still reach (ADR-0009 §1). The replica
    // evicts down to it and no further, so both sides offer the same undo.
    uint64_t undo_floor = 0;
    // Snapshot only: the id counter. The window holds it too; a replica
    // refuses a Snapshot whose fields and bytes disagree.
    uint64_t next_id = 0;
    std::string bytes;
    // Undo and Redo only: the label of the step undone or redone ("Paste",
    // "Record"), for the Edit menu's text. Display only. Protocol 4.
    std::string label;
    // SnapshotHeader only (protocol 10).
    uint64_t part_count = 0;
    uint64_t part_bytes = 0;
};

// --- the plugin catalog (ADR-0012 §8), protocol 3 ---------------------------

// Append-only enum. The model's PluginFormat spells the same two.
enum class PluginFormat : uint32_t { Lv2 = 0, Vst3 = 1 };

// How a plugin's code runs (ADR-0017 Y2). Append-only. Yabridge: a Windows
// VST3 bridged into a Linux bundle, running under Wine, which an interface
// shows as "Windows (Wine)". It is validated by the deadline there rather
// than by the buffer trap (Y4), and it costs a hop per block.
enum class PluginBridge : uint32_t { None = 0, Yabridge = 1 };

enum PluginParamFlag : uint32_t {
    kParamAutomatable = 1u << 0,
    kParamToggle = 1u << 1,
    kParamEnum = 1u << 2,
    kParamLogarithmic = 1u << 3,
    // Past the chain's kMaxParamsPerChain: not automatable from Sonora, still
    // saved in the plugin's own state.
    kParamPastCap = 1u << 4,
};
constexpr uint32_t kKnownParamFlags = (1u << 5) - 1;

struct PluginParam {
    // Dense; equal to the entry's place in `params`. The order to show them
    // in, and nothing more: a change names its parameter by `key`.
    uint32_t index = 0;
    std::string key;       // the stable key the session file stores (ADR-0012 §6.1)
    std::string name;
    // Format-native, as the value a PluginParameterChange carries.
    float min = 0.0f;
    float max = 1.0f;
    float default_value = 0.0f;
    uint32_t flags = 0;    // PluginParamFlag
    // What the value is in: "dB", "Hz", "ms", "%"; empty when the plugin
    // does not say (LV2 units:unit, VST3 ParameterInfo::units).
    std::string unit;
    // 0: continuous. Otherwise the number of values it takes, evenly spaced
    // from min to max: 2 for a toggle, the choices of an enum (LV2
    // enumeration scale points, VST3 stepCount + 1). Never 1.
    uint32_t steps = 0;
    // Empty, or one label per step, in order from min ("Off", "Peak", "RMS").
    std::vector<std::string> labels;
};

struct PluginInfo {
    PluginFormat format = PluginFormat::Lv2;
    std::string id;        // LV2 URI or VST3 class id: the model's PluginRef::id
    std::string name;
    std::string vendor;
    std::string version;
    // What a browser groups it by, as the plugin declares it, broadest
    // first: its LV2 classes' labels ("Dynamics", "Compressor"), or its VST3
    // subCategories split at '|' ("Fx", "Dynamics"). May be empty.
    std::vector<std::string> categories;
    uint32_t inputs = 0;   // audio channels
    uint32_t outputs = 0;
    // False when the scan refused the plugin; `refusal` says why, and an
    // AddInsert naming it is refused with the same words.
    bool usable = true;
    std::string refusal;
    bool has_editor = false;
    PluginBridge bridge = PluginBridge::None;
    std::vector<PluginParam> params;
};

// Every plugin the engine can host, as its last scan found them. Sent after
// the Snapshot when a client connects, and again after a rescan; each one
// replaces the last.
struct PluginCatalog {
    uint64_t generation = 0;   // bumped by every scan
    std::vector<PluginInfo> plugins;
};

// What one poll() hands over, appended in arrival order per kind.
struct Inbox {
    std::vector<CommandReply> replies;
    std::vector<EngineEvent> events;
    std::vector<ModelUpdate> model;
    std::vector<PluginCatalog> catalogs;

    void clear() {
        replies.clear();
        events.clear();
        model.clear();
        catalogs.clear();
    }
};

}  // namespace sonora::api
