// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chord -> command -> engine or interface. One key system: the chord comes
// from qt_keys.h, the profile and every rule about it from ui/keymap, the
// vocabulary from contracts/api. This file only decides where a resolved
// command goes and what the operator is told when it goes nowhere.
//
// Until M4 gives CommandSpec a domain (ADR-0007 §5), "interface" means a
// command installed in this router's own registry, and "engine" means one
// the engine says it implements. Neither: NotImplemented, said out loud.
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
    };
    Kind kind = Kind::Unbound;
    std::string command;
    CommandContext ctx;
    DispatchResult result = DispatchResult::Ok;
    uint64_t seq = 0;
    // What the status line should say, or empty when nothing went wrong.
    std::string message;
};

class KeyRouter {
public:
    using Send = std::function<uint64_t(const std::string&, const CommandContext&)>;

    // `engine_implemented` is api::Hello::implemented_commands.
    KeyRouter(KeyMap map, std::vector<std::string> engine_implemented, Send send);

    // The interface's own commands. Installed before prepare().
    CommandRegistry& local() { return local_; }

    // Runs ensure_essentials() against `device` and what this build
    // implements, engine and interface together, and builds the footer. Its
    // notices are returned for the operator, as the tty does.
    std::vector<std::string> prepare(const std::vector<KeyChord>& device);

    RouteResult route(const KeyChord& chord);
    // The same entry point for a command that did not come from a key: a
    // click on an arm button goes exactly where its key would.
    RouteResult invoke(const std::string& command, const CommandContext& ctx);

    bool implemented(const std::string& id) const;
    const KeyMap& map() const { return map_; }
    // "chord=command" entries, only for bindings the device can send and the
    // build implements, as engine/app's build_help() does for the tty.
    const std::vector<std::string>& help() const { return help_; }

private:
    KeyMap map_;
    std::vector<std::string> engine_;
    Send send_;
    CommandRegistry local_;
    std::vector<std::string> help_;
};

}  // namespace sonora::ui
