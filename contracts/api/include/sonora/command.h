// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command vocabulary every interface surface binds to, and the domain
// each command belongs to (ADR-0007 §5).
//
// Key maps are data (keymaps/*.json) and name commands by stable string id.
// Nothing here is reachable from Session::process(): it allocates, it holds
// std::function, it builds strings. The dispatcher runs on the GUI thread or
// in the terminal loop and reaches the audio thread only through the atomics
// on Track and Transport, which is the whole point of keeping it separate.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sonora {

// Some commands act on a track; most act on the session as a whole. This build
// has no track selection, so a command that wants a target and is invoked
// without one fails -- it does not guess a track number. When a selection
// exists, that is what a target-less invocation will act on; until then
// ensure_essentials() and the footer treat such a binding as unusable.
enum class CommandTarget {
    None,   // transport.play, edit.undo
    Track,  // track.arm, track.toggle_mute
};

// Who runs a command (ADR-0007 §5). An interface dispatches Interface
// commands to its own registry and sends Engine commands to the engine as
// api::CommandRequest; the engine installs handlers for Engine commands only.
// The default for anything not listed as Interface is Engine: what is saved
// in the session is engine state, how the operator looks at it is not.
enum class CommandDomain {
    Engine,     // transport.*, track.*, edit operations, nudge.*, app.save
    Interface,  // tool.*, zoom.*, edit.mode.*, focus.*, app.quit
};

struct CommandSpec {
    const char* id;
    CommandTarget target;
    const char* help;
    // Whether the command reads CommandContext::argument. False, an
    // "argument" in a profile is refused by name the way an out-of-place
    // "target" already is, instead of being accepted and then ignored for
    // ever. The plugin editor commands are the first to set it.
    bool takes_argument = false;
    CommandDomain domain = CommandDomain::Engine;
};

// Everything the interface may name. A key map is validated against this
// table, so a typo in a profile is a load failure rather than a dead key.
//
// Being in this table does not mean this build implements it. The two
// failure modes are deliberately different:
//
//   - a command *not in the table* is a typo, and the key map refuses to load;
//   - a command in the table with no handler registered is NotImplemented,
//     which the interface reports to the operator when the key is pressed.
//
// That split is what lets keymaps/protools.json ship the real Pro Tools
// bindings before the editor they drive exists, without ever letting a key
// silently do nothing.
inline const std::vector<CommandSpec>& command_catalog() {
    static const std::vector<CommandSpec> catalog = {
        // --- transport ---
        {"transport.play", CommandTarget::None, "Start playback"},
        {"transport.stop", CommandTarget::None, "Stop the transport"},
        {"transport.toggle_play", CommandTarget::None, "Play, or stop if rolling"},
        {"transport.record", CommandTarget::None, "Start recording"},
        {"transport.toggle_record", CommandTarget::None,
         "Start recording, or stop and close the take if recording"},
        {"transport.return_to_zero", CommandTarget::None,
         "Move the playhead to the session start"},
        {"transport.go_to_end", CommandTarget::None,
         "Move the playhead to the session end"},
        {"transport.rewind", CommandTarget::None, "Rewind"},
        {"transport.fast_forward", CommandTarget::None, "Fast forward"},
        {"transport.toggle_loop", CommandTarget::None, "Toggle loop playback"},
        {"transport.play_half_speed", CommandTarget::None, "Play at half speed"},

        // --- tracks ---
        {"track.arm", CommandTarget::Track, "Record-arm a track"},
        {"track.disarm", CommandTarget::Track, "Record-disarm a track"},
        {"track.toggle_arm", CommandTarget::Track, "Toggle record arm on a track"},
        {"track.arm_all", CommandTarget::None, "Record-arm every track"},
        {"track.disarm_all", CommandTarget::None, "Record-disarm every track"},
        {"track.toggle_mute", CommandTarget::Track, "Toggle mute on a track"},
        {"track.toggle_solo", CommandTarget::Track, "Toggle solo on a track"},
        {"track.toggle_input_monitor", CommandTarget::Track,
         "Toggle input monitoring on a track"},
        {"track.select", CommandTarget::Track, "Make a track the selected track"},
        {"track.select_next", CommandTarget::None, "Select the next track down"},
        {"track.select_previous", CommandTarget::None, "Select the next track up"},

        // --- edit modes (ADR-0004: workflow, reproduced deliberately) ---
        {"edit.mode.shuffle", CommandTarget::None, "Shuffle edit mode",
         false, CommandDomain::Interface},
        {"edit.mode.slip", CommandTarget::None, "Slip edit mode",
         false, CommandDomain::Interface},
        {"edit.mode.spot", CommandTarget::None, "Spot edit mode",
         false, CommandDomain::Interface},
        {"edit.mode.grid", CommandTarget::None, "Grid edit mode",
         false, CommandDomain::Interface},
        {"edit.mode.next", CommandTarget::None, "Cycle through the edit modes",
         false, CommandDomain::Interface},

        // --- tools ---
        {"tool.smart", CommandTarget::None, "Smart Tool",
         false, CommandDomain::Interface},
        {"tool.zoom", CommandTarget::None, "Zoomer tool",
         false, CommandDomain::Interface},
        {"tool.trim", CommandTarget::None, "Trim tool",
         false, CommandDomain::Interface},
        {"tool.select", CommandTarget::None, "Selector tool",
         false, CommandDomain::Interface},
        {"tool.grab", CommandTarget::None, "Grabber tool",
         false, CommandDomain::Interface},
        {"tool.scrub", CommandTarget::None, "Scrubber tool",
         false, CommandDomain::Interface},
        {"tool.pencil", CommandTarget::None, "Pencil tool",
         false, CommandDomain::Interface},
        {"tool.next", CommandTarget::None, "Cycle through the tools",
         false, CommandDomain::Interface},

        // --- zoom ---
        {"zoom.in", CommandTarget::None, "Zoom in horizontally",
         false, CommandDomain::Interface},
        {"zoom.out", CommandTarget::None, "Zoom out horizontally",
         false, CommandDomain::Interface},
        {"zoom.vertical_in", CommandTarget::None, "Zoom in vertically",
         false, CommandDomain::Interface},
        {"zoom.vertical_out", CommandTarget::None, "Zoom out vertically",
         false, CommandDomain::Interface},
        {"zoom.toggle", CommandTarget::None, "Toggle between two zoom states",
         false, CommandDomain::Interface},
        {"zoom.fit_session", CommandTarget::None, "Fit the session in the window",
         false, CommandDomain::Interface},
        {"zoom.fit_selection", CommandTarget::None, "Fit the selection in the window",
         false, CommandDomain::Interface},

        // --- nudge ---
        {"nudge.forward", CommandTarget::None, "Nudge forward by the nudge value"},
        {"nudge.back", CommandTarget::None, "Nudge back by the nudge value"},

        // --- edit operations ---
        {"edit.undo", CommandTarget::None, "Undo"},
        {"edit.redo", CommandTarget::None, "Redo"},
        {"edit.cut", CommandTarget::None, "Cut"},
        {"edit.copy", CommandTarget::None, "Copy"},
        {"edit.paste", CommandTarget::None, "Paste"},
        {"edit.clear", CommandTarget::None, "Clear the selection"},
        {"edit.duplicate", CommandTarget::None, "Duplicate the selection"},
        {"edit.repeat", CommandTarget::None, "Repeat the selection"},
        {"edit.select_all", CommandTarget::None, "Select all"},
        {"edit.trim_start_to_insertion", CommandTarget::None,
         "Trim the region start to the insertion point"},
        {"edit.trim_end_to_insertion", CommandTarget::None,
         "Trim the region end to the insertion point"},
        {"edit.separate_at_selection", CommandTarget::None,
         "Separate the region at the selection"},
        {"edit.heal_separation", CommandTarget::None, "Heal a separation"},
        {"edit.consolidate", CommandTarget::None, "Consolidate the selection"},
        {"edit.fade_to_start", CommandTarget::None,
         "Fade from the region start to the insertion point"},
        {"edit.fade_to_end", CommandTarget::None,
         "Fade from the insertion point to the region end"},
        {"edit.fade_selection", CommandTarget::None, "Fade or crossfade the selection"},

        // --- plugins (ADR-0012 §8) ---
        {"plugin.rescan", CommandTarget::None, "Scan the installed plugins again"},
        // Argument: "<slot> <parent token>", the slot 0..9 (A..J) and the
        // window to parent the editor to, opaque, empty on the appliance.
        {"plugin.editor.open", CommandTarget::Track, "Open a plugin's own editor", true},
        {"plugin.editor.close", CommandTarget::Track, "Close a plugin's own editor", true},
        {"plugin.retry", CommandTarget::Track,
         "Start a track's plugin chain again after it was left down"},

        // --- keyboard focus ---
        // Pro Tools' Commands Keyboard Focus: whether the single-letter
        // bindings a profile marks "focus": "commands" run. Which keys mean
        // what is the interface's, never the session's.
        {"focus.commands", CommandTarget::None, "Toggle Commands Keyboard Focus",
         false, CommandDomain::Interface},

        // --- windows ---
        {"window.toggle_mix_edit", CommandTarget::None,
         "Switch between the Edit and Mix windows"},

        // --- application ---
        {"app.save", CommandTarget::None, "Save the session"},
        {"app.quit", CommandTarget::None, "Leave the engine",
         false, CommandDomain::Interface},
    };
    return catalog;
}

inline const CommandSpec* find_command(const std::string& id) {
    for (const auto& spec : command_catalog()) {
        if (id == spec.id) return &spec;
    }
    return nullptr;
}

// Whether `id` is a catalog command the engine runs. False for an id not in
// the catalog at all.
inline bool is_engine_command(const std::string& id) {
    const CommandSpec* spec = find_command(id);
    return spec != nullptr && spec->domain == CommandDomain::Engine;
}

// What a command was invoked with. Commands that do not take a target ignore
// all of it. A negative target means none was given -- never track 0 -- and
// with no selection in this build a track command then has nothing to act on.
struct CommandContext {
    int target = -1;
    std::string argument;  // free-form, for the few commands that need a value

    bool has_target() const { return target >= 0; }
};

// Returns false when the command could not do its job, which the interface
// reports. It must never block: this runs on the GUI thread.
using CommandHandler = std::function<bool(const CommandContext&)>;
// The same, for a handler that can say why it refused. "The command failed"
// tells an operator nothing; "arming cannot change during a take" tells them
// what to do. `why` is left empty on success.
using ExplainingHandler =
    std::function<bool(const CommandContext&, std::string& why)>;

enum class DispatchResult {
    Ok,
    UnknownCommand,   // not in the catalog at all; a key map should never
                      // produce this, because loading validates first
    NotImplemented,   // in the catalog, nothing wired up in this build
    Failed,           // the handler ran and said no
};

inline const char* dispatch_result_text(DispatchResult r) {
    switch (r) {
        case DispatchResult::Ok: return "ok";
        case DispatchResult::UnknownCommand: return "no such command";
        case DispatchResult::NotImplemented: return "not implemented in this build";
        case DispatchResult::Failed: return "the command failed";
    }
    return "unknown";
}

class CommandRegistry {
public:
    // Fails if id is not in the catalog, so a handler for a misspelled
    // command is caught at startup instead of never firing.
    bool install(const std::string& id, CommandHandler handler) {
        return install(id, ExplainingHandler(
                               [h = std::move(handler)](const CommandContext& ctx,
                                                        std::string&) {
                                   return h(ctx);
                               }));
    }
    bool install(const std::string& id, ExplainingHandler handler) {
        if (find_command(id) == nullptr) return false;
        handlers_[id] = std::move(handler);
        return true;
    }

    bool implemented(const std::string& id) const {
        return handlers_.find(id) != handlers_.end();
    }

    std::size_t implemented_count() const { return handlers_.size(); }

    // `why` is the handler's reason when the result is Failed, and may be
    // empty if it gave none; dispatch_result_text() is the fallback.
    DispatchResult dispatch(const std::string& id, const CommandContext& ctx,
                            std::string& why) const {
        why.clear();
        if (find_command(id) == nullptr) return DispatchResult::UnknownCommand;
        const auto it = handlers_.find(id);
        if (it == handlers_.end()) return DispatchResult::NotImplemented;
        if (it->second(ctx, why)) {
            why.clear();
            return DispatchResult::Ok;
        }
        return DispatchResult::Failed;
    }

    DispatchResult dispatch(const std::string& id, const CommandContext& ctx) const {
        std::string why;
        return dispatch(id, ctx, why);
    }

    DispatchResult dispatch(const std::string& id) const {
        return dispatch(id, CommandContext{});
    }

private:
    std::unordered_map<std::string, ExplainingHandler> handlers_;
};

}  // namespace sonora
