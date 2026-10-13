// SPDX-License-Identifier: GPL-3.0-or-later
#include "key_router.h"

#include <algorithm>
#include <utility>

namespace sonora::ui {

namespace {

std::string short_name(const std::string& command) {
    const std::size_t dot = command.rfind('.');
    return dot == std::string::npos ? command : command.substr(dot + 1);
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

KeyRouter::KeyRouter(KeyMap map, std::vector<std::string> engine_implemented, Send send)
    : map_(std::move(map)), engine_(std::move(engine_implemented)), send_(std::move(send)) {}

bool KeyRouter::implemented(const std::string& id) const {
    return local_.implemented(id) ||
           std::find(engine_.begin(), engine_.end(), id) != engine_.end();
}

std::vector<std::string> KeyRouter::prepare(const std::vector<KeyChord>& device) {
    const auto impl = [this](const std::string& id) { return implemented(id); };
    std::vector<std::string> notices = map_.ensure_essentials(device, impl);

    help_.clear();
    for (const auto& b : map_.bindings()) {
        if (!implemented(b.command) || needs_selection(b)) continue;
        if (std::find(device.begin(), device.end(), b.chord) == device.end()) continue;
        std::string e = KeyMap::chord_to_string(b.chord) + "=" + short_name(b.command);
        if (b.ctx.has_target()) e += std::to_string(b.ctx.target + 1);
        help_.push_back(std::move(e));
    }
    if (help_.empty()) {
        help_.push_back("no key in this profile does anything this build implements");
    }
    return notices;
}

RouteResult KeyRouter::route(const KeyChord& chord) {
    const Binding* b = map_.resolve(chord);
    if (b == nullptr) {
        RouteResult r;
        r.kind = RouteResult::Kind::Unbound;
        r.message = KeyMap::chord_to_string(chord) + " is not bound";
        return r;
    }
    return invoke(b->command, b->ctx);
}

RouteResult KeyRouter::invoke(const std::string& command, const CommandContext& ctx) {
    RouteResult r;
    r.command = command;
    r.ctx = ctx;
    if (local_.implemented(command)) {
        std::string why;
        r.kind = RouteResult::Kind::Local;
        r.result = local_.dispatch(command, ctx, why);
        if (r.result != DispatchResult::Ok) {
            r.message = command + ": " + (why.empty() ? dispatch_result_text(r.result) : why);
        }
        return r;
    }
    if (std::find(engine_.begin(), engine_.end(), command) != engine_.end()) {
        r.kind = RouteResult::Kind::Sent;
        r.seq = send_(command, ctx);
        return r;
    }
    r.kind = RouteResult::Kind::NotImplemented;
    r.result = find_command(command) ? DispatchResult::NotImplemented
                                     : DispatchResult::UnknownCommand;
    r.message = command + ": " + dispatch_result_text(r.result);
    return r;
}

}  // namespace sonora::ui
