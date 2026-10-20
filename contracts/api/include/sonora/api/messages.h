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
constexpr uint32_t kProtocolVersion = 6;

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
};

struct ParameterChange {
    uint64_t seq = 0;
    uint32_t track = 0;
    ParamId param = ParamId::Gain;
    float value = 0.0f;
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
    // Open another session: `op` is a session file's bytes (common/model's
    // session_file.h). The engine's document becomes it, with a new undo
    // history, and every replica gets a Snapshot. Bytes rather than a path,
    // so the engine needs no view of the interface's filesystem and File >
    // Open's checks stay where the dialog is. Protocol version 2.
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
};

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
};

struct EngineEvent {
    EventKind kind = EventKind::Notice;
    int32_t track = -1;
    // TakeFailed: the take number. 0 otherwise.
    uint64_t frame = 0;
    // PluginFault: the insert's model id. 0 otherwise. Protocol 3.
    uint64_t insert = 0;
    std::string text;
};

// One change to the document, sent to every replica in revision order.
// Append-only enum.
enum class ModelUpdateKind : uint32_t {
    Op = 0,        // apply `bytes` (one encoded operation)
    Undo = 1,      // move the replica's own pointer back one step
    Redo = 2,      // and forward
    // Replace the replica with `bytes`: the document and its whole undo
    // window (common/model's encode_window), so undo and redo after it
    // apply on the replica as on the engine. Sent after Hello and on Resync.
    Snapshot = 3,
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
};

// --- the plugin catalog (ADR-0012 §8), protocol 3 ---------------------------

// Append-only enum. The model's PluginFormat spells the same two.
enum class PluginFormat : uint32_t { Lv2 = 0, Vst3 = 1 };

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
