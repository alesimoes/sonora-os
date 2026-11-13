// SPDX-License-Identifier: GPL-3.0-or-later
// The editor commands and event (plugin_editor_routes.h).
#include "sonora/plugin_editor_routes.h"

#include "sonora/api/plugin_editor.h"
#include "sonora/api/wire.h"
#include "sonora/engine_control_server.h"
#include "sonora/plugin_editor_control.h"

namespace sonora {

namespace {

// The registry hands a handler the context, not the request; rebuilt, the
// wire's own parser checks it, so the engine and the codec agree on what a
// slot is.
api::CommandRequest request_of(const char* id, const CommandContext& ctx) {
    api::CommandRequest r;
    r.command = id;
    r.target = ctx.target;
    r.argument = ctx.argument;
    return r;
}

}  // namespace

void install_plugin_editor_routes(CommandRegistry& registry, editor_service::PluginEditorService& editors) {
    registry.install(api::kCommandOpenPluginEditor,
                     ExplainingHandler([&editors](const CommandContext& ctx, std::string& why) {
                         api::OpenPluginEditor m;
                         if (!api::from_command(request_of(api::kCommandOpenPluginEditor, ctx), m, why))
                             return false;
                         editors.open(m.track, m.slot);
                         return true;
                     }));
    registry.install(api::kCommandClosePluginEditor,
                     ExplainingHandler([&editors](const CommandContext& ctx, std::string& why) {
                         api::ClosePluginEditor m;
                         if (!api::from_command(request_of(api::kCommandClosePluginEditor, ctx), m, why))
                             return false;
                         editors.close(m.track, m.slot);
                         return true;
                     }));
}

api::EngineEvent plugin_editor_event(uint32_t track, uint32_t slot, const editor_service::EditorStatus& status) {
    api::PluginEditorState s;
    s.track = track;
    s.slot = slot;
    // The two enums are numbered alike (api/plugin_editor.h).
    s.state = static_cast<api::PluginEditorStatus>(status.state);
    if (s.state == api::PluginEditorStatus::Failed) s.reason = status.reason;
    return api::to_event(s);
}

// ---- the server's half: kept here so the server's file does not grow.

void EngineControlServer::install_plugin_editors() {
    editor_port_ = make_hosting_editor_port([this]() { return hosting_.get(); },
                                            [this]() -> const model::Snapshot& { return document_->current(); });
    editors_ = std::make_unique<PluginEditorControl>(*editor_port_);
    // Every change, to every client attached now (ADR-0088 decision 7); one
    // that attaches later is sent the standing ones by greet_editors().
    editors_->set_listener([this](uint32_t track, uint32_t slot, const editor_service::EditorStatus& s) {
        post_transient(plugin_editor_event(track, slot, s));
    });
    install_plugin_editor_routes(registry_, *editors_);
}

void EngineControlServer::greet_editors(Client& client) {
    if (!editors_) return;
    for (const PluginEditorControl::Entry& e : editors_->standing()) {
        std::string bytes;
        api::wire::encode(plugin_editor_event(e.track, e.slot, e.status), bytes);
        send_to(client, bytes);
    }
}

}  // namespace sonora
