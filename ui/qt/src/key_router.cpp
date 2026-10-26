// SPDX-License-Identifier: GPL-3.0-or-later
#include "key_router.h"

#include <algorithm>
#include <utility>

#include "sonora/api/messages.h"

namespace sonora::ui {

namespace {

std::string short_name(const std::string& command) {
    const std::size_t dot = command.rfind('.');
    return dot == std::string::npos ? command : command.substr(dot + 1);
}

bool is_interface(const CommandSpec* spec) {
    return spec != nullptr && spec->domain == CommandDomain::Interface;
}

}  // namespace

KeyMap keymap_or_built_in(const std::string& path, std::string& refusal) {
    refusal.clear();
    if (path.empty()) return KeyMap::built_in_default();
    KeyMap loaded;
    std::string err;
    if (KeyMap::load_file(path, loaded, err)) return loaded;
    refusal = "key map refused, using the built-in keys: " + err;
    return KeyMap::built_in_default();
}

std::string play_range_argument(int64_t start, int64_t end) {
    if (start < 0 || end <= start) return {};
    return std::to_string(start) + "-" + std::to_string(end);
}

KeyRouter::KeyRouter(KeyMap map, std::vector<std::string> engine_implemented, Send send)
    : map_(std::move(map)),
      profile_(map_),
      engine_(std::move(engine_implemented)),
      send_(std::move(send)) {
    local_.install("focus.commands", [this](const CommandContext&) {
        setCommandsFocus(!commands_focus_);
        return true;
    });
}

bool KeyRouter::implemented(const std::string& id) const {
    if (local_.implemented(id)) return true;
    // An Interface command the engine lists is still not the engine's to run.
    if (is_interface(find_command(id))) return false;
    return std::find(engine_.begin(), engine_.end(), id) != engine_.end();
}

std::vector<std::string> KeyRouter::prepare(const std::vector<KeyChord>& device) {
    device_ = device;
    prepared_ = true;
    map_ = profile_;
    const auto impl = [this](const std::string& id) { return implemented(id); };
    std::vector<std::string> notices = map_.ensure_essentials(device_, impl);
    buildHelp();
    return notices;
}

std::vector<std::string> KeyRouter::setMap(KeyMap map) {
    profile_ = std::move(map);
    return prepare(device_);
}

std::vector<std::string> KeyRouter::setEngineCommands(std::vector<std::string> engine_implemented) {
    engine_ = std::move(engine_implemented);
    if (!prepared_) return {};
    return prepare(device_);
}

void KeyRouter::setCommandsFocus(bool on) {
    if (commands_focus_ == on) return;
    commands_focus_ = on;
    buildHelp();
}

bool KeyRouter::hasFocusKeys() const {
    for (const auto& b : map_.bindings()) {
        if (needs_commands_focus(b)) return true;
    }
    return false;
}

std::string KeyRouter::focusToggleChord() const {
    for (const auto& b : map_.bindings()) {
        if (b.command == "focus.commands" &&
            std::find(device_.begin(), device_.end(), b.chord) != device_.end()) {
            return KeyMap::chord_to_string(b.chord);
        }
    }
    return std::string();
}

void KeyRouter::buildHelp() {
    help_.clear();
    for (const auto& b : map_.bindings()) {
        if (!implemented(b.command) || needs_selection(b)) continue;
        if (needs_commands_focus(b) && !commands_focus_) continue;
        if (std::find(device_.begin(), device_.end(), b.chord) == device_.end()) continue;
        std::string e = KeyMap::chord_to_string(b.chord) + "=" + short_name(b.command);
        if (b.ctx.has_target()) e += std::to_string(b.ctx.target + 1);
        help_.push_back(std::move(e));
    }
    if (help_.empty()) {
        help_.push_back("no key in this profile does anything this build implements");
    }
}

RouteResult KeyRouter::route(const KeyChord& chord) {
    const Binding* b = map_.resolve(chord);
    if (b == nullptr) {
        RouteResult r;
        r.kind = RouteResult::Kind::Unbound;
        r.message = KeyMap::chord_to_string(chord) + " is not bound";
        return r;
    }
    if (needs_commands_focus(*b) && !commands_focus_) {
        // Not silently nothing: the key is the profile's, and the operator
        // is told what it would do and how to make it do it.
        RouteResult r;
        r.kind = RouteResult::Kind::FocusOff;
        r.command = b->command;
        r.ctx = b->ctx;
        const std::string toggle = focusToggleChord();
        r.message = KeyMap::chord_to_string(chord) + " is " + b->command +
                    " only with Commands Keyboard Focus, which is off; " +
                    (toggle.empty() ? std::string("the a-z button turns it on")
                                    : toggle + " or the a-z button turns it on");
        return r;
    }
    return invoke(b->command, b->ctx);
}

RouteResult KeyRouter::invoke(const std::string& command, const CommandContext& given) {
    CommandContext ctx = given;
    const CommandSpec* spec = find_command(command);
    if (spec && spec->target == CommandTarget::Track && !ctx.has_target() && selected_) {
        ctx.target = selected_();
    }
    RouteResult r;
    r.command = command;
    r.ctx = ctx;
    const auto run_local = [&]() {
        std::string why;
        r.kind = RouteResult::Kind::Local;
        r.result = local_.dispatch(command, ctx, why);
        if (r.result != DispatchResult::Ok) {
            r.message = command + ": " + (why.empty() ? dispatch_result_text(r.result) : why);
        }
        return r;
    };
    // The engine is the authority on what it runs: once it implements an
    // Engine command, the window's stand-in for it is no longer used.
    if (spec != nullptr && !is_interface(spec) &&
        std::find(engine_.begin(), engine_.end(), command) != engine_.end()) {
        if ((command == "transport.play" || command == "transport.toggle_play") &&
            ctx.argument.empty() && play_range_ &&
            std::find(caps_.begin(), caps_.end(), api::kCapabilityPlayRange) != caps_.end()) {
            ctx.argument = play_range_();
            r.ctx = ctx;
        }
        // Pro Tools' Loop Playback and Loop Record loop the selection; an
        // engine that runs them takes it, so no capability is asked. Logic's
        // Cycle keeps a cycle area already set, and the engine decides that:
        // it reads the selection only when none is, as Cmd+U always does.
        if ((command == "transport.toggle_loop" || command == "transport.toggle_loop_record" ||
             command == "transport.toggle_cycle" || command == "transport.cycle_selection") &&
            ctx.argument.empty() && play_range_) {
            ctx.argument = play_range_();
            r.ctx = ctx;
        }
        r.kind = RouteResult::Kind::Sent;
        r.seq = send_(command, ctx);
        return r;
    }
    if (local_.implemented(command)) return run_local();
    r.kind = RouteResult::Kind::NotImplemented;
    r.result = spec ? DispatchResult::NotImplemented : DispatchResult::UnknownCommand;
    r.message = command + ": " + dispatch_result_text(r.result);
    return r;
}

}  // namespace sonora::ui
