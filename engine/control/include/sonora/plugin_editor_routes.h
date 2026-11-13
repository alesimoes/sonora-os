// SPDX-License-Identifier: GPL-3.0-or-later
//
// plugin.editor.open and plugin.editor.close on the wire (ADR-0088,
// amendment of 2026-11-13; contracts/api plugin_editor.h, protocol 70).
// Control thread only.
#pragma once

#include <cstdint>

#include "sonora/api/messages.h"
#include "sonora/command.h"
#include "sonora/editor_service/plugin_editor_service.h"

namespace sonora {

// Installs both commands on `registry`, each parsed by contracts/api's
// from_command and run on `editors`, which outlives the registry's use.
// The reply is Ok once the request is taken, whatever the editor then does:
// the outcome, Failed with its reason included, is the PluginEditorState
// event. A request the wire refuses (no track, a bad slot) fails with why.
void install_plugin_editor_routes(CommandRegistry& registry, editor_service::PluginEditorService& editors);

// The PluginEditorState event of one editor's state.
api::EngineEvent plugin_editor_event(uint32_t track, uint32_t slot, const editor_service::EditorStatus& status);

}  // namespace sonora
