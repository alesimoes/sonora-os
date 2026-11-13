// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plug-in's own editor on the wire (ADR-0088, protocol 70).
//
//   Command  "plugin.editor_open"   CommandRequest::target = the track index,
//                                   argument "slot=<n>" (0 is slot A)
//   Command  "plugin.editor_close"  the same
//   Event    EventKind::PluginEditor on every state change, to every client:
//              track  = the track index
//              insert = the slot index
//              flags  = PluginEditorStatus
//              text   = Failed: the host's reason, unchanged; else empty
//            A client that attaches later is sent every editor that is not
//            Closed. Opening a session closes every editor of the last one.
//
// The UI names an insert by {track, slot} and never by host, instance or
// window id: only the engine knows which host runs it (ADR-0088 §1). Open on
// an editor that is Opening or Open raises it and changes nothing; close on
// one that is Closed or Failed is ignored. Failed is a held notice the UI
// shows with the generic editor in its place, never a blank window.
//
// These are CommandRequest and EngineEvent words rather than wire types of
// their own, so the codec, the reply and the command map are the ones every
// other command uses; this header only says what the fields mean.
#pragma once

#include <cstdint>
#include <string>

#include "sonora/api/messages.h"

namespace sonora::api {

inline constexpr const char* kCommandOpenPluginEditor = "plugin.editor_open";
inline constexpr const char* kCommandClosePluginEditor = "plugin.editor_close";

// The same four states as editor_service::EditorState, numbered alike.
// Append-only.
enum class PluginEditorStatus : uint32_t { Closed = 0, Opening = 1, Open = 2, Failed = 3 };
inline constexpr uint32_t kLastPluginEditorStatus =
    static_cast<uint32_t>(PluginEditorStatus::Failed);

struct OpenPluginEditor {
    uint32_t track = 0;  // track index
    uint32_t slot = 0;   // insert slot, 0 .. kInsertSlots - 1
};

struct ClosePluginEditor {
    uint32_t track = 0;
    uint32_t slot = 0;
};

struct PluginEditorState {
    uint32_t track = 0;
    uint32_t slot = 0;
    PluginEditorStatus state = PluginEditorStatus::Closed;
    std::string reason;  // Failed only
};

CommandRequest to_command(const OpenPluginEditor& m);
CommandRequest to_command(const ClosePluginEditor& m);
// False, `why` set, for another command, a missing or negative track, or a
// slot that is not a whole number below kInsertSlots.
bool from_command(const CommandRequest& r, OpenPluginEditor& out, std::string& why);
bool from_command(const CommandRequest& r, ClosePluginEditor& out, std::string& why);

EngineEvent to_event(const PluginEditorState& m);
// False, `why` set, for another kind, a state this version does not define,
// a negative track, a slot out of range, or a reason on a state other than
// Failed (a reason the UI would not show is a reason it would lose).
bool from_event(const EngineEvent& e, PluginEditorState& out, std::string& why);

}  // namespace sonora::api
