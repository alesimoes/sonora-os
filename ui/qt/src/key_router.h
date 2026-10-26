// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chord -> command -> engine or interface. One key system: the chord comes
// from qt_keys.h, the profile and every rule about it from ui/keymap, the
// vocabulary from contracts/api. This file only decides where a resolved
// command goes and what the operator is told when it goes nowhere.
//
// Where a command goes is its CommandSpec::domain (ADR-0007 §5). An
// Interface command runs in this router's own registry and never reaches the
// engine. An Engine command goes to the engine on the control API (M4) when
// the engine says it implements it; until it does, a handler the window
// installed for it stands in (the Edit window's edits, through the replica's
// model requests). Neither: NotImplemented, said out loud.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/command.h"
#include "sonora/keymap.h"

namespace sonora::ui {

struct RouteResult {
    enum class Kind {
        Unbound,         // no binding for this chord
        Local,           // ran in the interface; `result` says how it went
        Sent,            // queued to the engine as `seq`; the reply says more
        NotImplemented,  // in the catalog, implemented nowhere in this build
        FocusOff,        // a Commands Keyboard Focus key, and focus is off
    };
    Kind kind = Kind::Unbound;
    std::string command;
    CommandContext ctx;
    DispatchResult result = DispatchResult::Ok;
    uint64_t seq = 0;
    // What the status line should say, or empty when nothing went wrong.
    std::string message;
};

// The profile the interface runs on: the file at `path`, or the built-in
// default when `path` is empty. A profile that fails to load is refused
// whole, as at the tty -- nothing is partially applied. But this process has
// no shell behind it, and exiting would leave a black screen and a restart
// loop, so a refused profile gives the built-in default, and `refusal` the
// line the footer holds for as long as the interface runs, naming the file
// and the reason. `refusal` is empty when nothing was refused.
KeyMap keymap_or_built_in(const std::string& path, std::string& refusal);

// transport.play's "A-B" argument for the frames [start, end), or "" when
// that is no range to play.
std::string play_range_argument(int64_t start, int64_t end);

class KeyRouter {
public:
    using Send = std::function<uint64_t(const std::string&, const CommandContext&)>;

    // `engine_implemented` is api::Hello::implemented_commands, replaced by
    // setEngineCommands() on each new Hello. The router installs
    // focus.commands in its own registry: the focus is its state.
    KeyRouter(KeyMap map, std::vector<std::string> engine_implemented, Send send);
    // focus.commands' handler holds `this`.
    KeyRouter(const KeyRouter&) = delete;
    KeyRouter& operator=(const KeyRouter&) = delete;

    // Who a track command given no track acts on: the selected track, or -1
    // when nothing is selected (and the command then fails, as before).
    void setSelection(std::function<int()> selected) { selected_ = std::move(selected); }
    // What transport.play and transport.toggle_play play when given no
    // argument: play_range_argument() of the Edit selection, "" for none.
    // Sent only to an engine whose Hello announces api::kCapabilityPlayRange;
    // an older one would ignore it and play on past the selection.
    // transport.toggle_loop gets it too, always: it becomes the loop range.
    void setPlayRange(std::function<std::string()> range) { play_range_ = std::move(range); }
    // api::Hello::capabilities, replaced on each new Hello.
    void setEngineCapabilities(std::vector<std::string> caps) { caps_ = std::move(caps); }
    const std::vector<std::string>& engineCapabilities() const { return caps_; }

    // The interface's own commands. Installed before prepare().
    CommandRegistry& local() { return local_; }

    // Runs ensure_essentials() against `device` and what this build
    // implements, engine and interface together, and builds the footer. Its
    // notices are returned for the operator, as the tty does.
    std::vector<std::string> prepare(const std::vector<KeyChord>& device);

    // Another profile, at run time, after prepare(): the same checks and
    // repairs against the same device, and its notices. The focus state is
    // kept; it is the operator's, not the profile's.
    std::vector<std::string> setMap(KeyMap map);

    // The engine's list from a new Hello: an engine restarted as another
    // build may run commands the first did not, or no longer run one the
    // window has a stand-in for. The profile is checked and repaired again
    // from the file as loaded, not from the last repair, and its notices
    // returned, as setMap's are. Before prepare() it only stores the list.
    std::vector<std::string> setEngineCommands(std::vector<std::string> engine_implemented);
    const std::vector<std::string>& engineCommands() const { return engine_; }

    RouteResult route(const KeyChord& chord);
    // The same entry point for a command that did not come from a key: a
    // click on an arm button goes exactly where its key would.
    RouteResult invoke(const std::string& command, const CommandContext& ctx);

    bool implemented(const std::string& id) const;
    const KeyMap& map() const { return map_; }
    // "chord=command" entries, only for bindings the device can send, the
    // build implements, and the focus state lets run, as engine/app's
    // build_help() does for the tty.
    const std::vector<std::string>& help() const { return help_; }

    // Pro Tools' Commands Keyboard Focus. On at start: the letters worked
    // unconditionally before there was a toggle, and Avid does not say
    // which state a new session opens in.
    bool commandsFocus() const { return commands_focus_; }
    void setCommandsFocus(bool on);
    // Whether this profile has any key that needs focus; a profile without
    // one has nothing for an a-z button to change.
    bool hasFocusKeys() const;
    // The chord that toggles focus in this profile, "" when it has none.
    std::string focusToggleChord() const;

private:
    void buildHelp();

    KeyMap map_;
    KeyMap profile_;   // map_ before ensure_essentials() repaired it
    bool prepared_ = false;
    std::vector<std::string> engine_;
    Send send_;
    CommandRegistry local_;
    std::vector<std::string> help_;
    std::vector<KeyChord> device_;
    std::function<int()> selected_;
    std::function<std::string()> play_range_;
    std::vector<std::string> caps_;
    bool commands_focus_ = true;
};

}  // namespace sonora::ui
