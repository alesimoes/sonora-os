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

// Bump on any change to a message's encoding. A client and an engine that
// disagree refuse each other at connect, by name, with both numbers.
constexpr uint32_t kProtocolVersion = 1;

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
};

struct EngineEvent {
    EventKind kind = EventKind::Notice;
    int32_t track = -1;
    uint64_t frame = 0;
    std::string text;
};

// One change to the document, sent to every replica in revision order.
// Append-only enum.
enum class ModelUpdateKind : uint32_t {
    Op = 0,        // apply `bytes` (one encoded operation)
    Undo = 1,      // move the replica's own pointer back one step
    Redo = 2,      // and forward
    Snapshot = 3,  // replace the replica with `bytes`, a session file
};

struct ModelUpdate {
    ModelUpdateKind kind = ModelUpdateKind::Op;
    // The revision the document has after this update. A replica that
    // expected another one has missed something and asks to Resync.
    uint64_t revision = 0;
    // The oldest revision undo can still reach (ADR-0009 §1). The replica
    // evicts down to it and no further, so both sides offer the same undo.
    uint64_t undo_floor = 0;
    // Snapshot only: the id counter, which a session file does not hold.
    uint64_t next_id = 0;
    std::string bytes;
};

// What one poll() hands over, appended in arrival order per kind.
struct Inbox {
    std::vector<CommandReply> replies;
    std::vector<EngineEvent> events;
    std::vector<ModelUpdate> model;

    void clear() {
        replies.clear();
        events.clear();
        model.clear();
    }
};

}  // namespace sonora::api
