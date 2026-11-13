// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/plugin_editor.h"

#include <cstdlib>
#include <utility>

#include "sonora/api/state_block.h"

namespace sonora::api {
namespace {

CommandRequest request(const char* id, uint32_t track, uint32_t slot) {
    CommandRequest r;
    r.command = id;
    r.target = static_cast<int32_t>(track);
    r.argument = "slot=" + std::to_string(slot);
    return r;
}

bool read_slot(const std::string& arg, uint32_t& slot, std::string& why) {
    static const std::string kKey = "slot=";
    const std::string v = arg.compare(0, kKey.size(), kKey) == 0 ? arg.substr(kKey.size()) : "";
    // Two digits at most: kInsertSlots is 10, and strtoul cannot overflow.
    char* end = nullptr;
    const unsigned long n = std::strtoul(v.c_str(), &end, 10);
    if (v.empty() || v.size() > 2 || *end != '\0' || v[0] < '0' || v[0] > '9') {
        why = "name the insert: slot=<0.." + std::to_string(kInsertSlots - 1) + ">, not \"" +
              arg + "\"";
        return false;
    }
    if (n >= kInsertSlots) {
        why = "slot " + std::to_string(n) + " is past the last insert slot (" +
              std::to_string(kInsertSlots - 1) + ")";
        return false;
    }
    slot = static_cast<uint32_t>(n);
    return true;
}

bool read(const CommandRequest& r, const char* id, uint32_t& track, uint32_t& slot,
          std::string& why) {
    if (r.command != id) {
        why = "\"" + r.command + "\" is not " + id;
        return false;
    }
    if (r.target < 0) {
        why = std::string(id) + " needs a track";
        return false;
    }
    if (!read_slot(r.argument, slot, why)) return false;
    track = static_cast<uint32_t>(r.target);
    return true;
}

}  // namespace

CommandRequest to_command(const OpenPluginEditor& m) {
    return request(kCommandOpenPluginEditor, m.track, m.slot);
}

CommandRequest to_command(const ClosePluginEditor& m) {
    return request(kCommandClosePluginEditor, m.track, m.slot);
}

bool from_command(const CommandRequest& r, OpenPluginEditor& out, std::string& why) {
    OpenPluginEditor m;
    if (!read(r, kCommandOpenPluginEditor, m.track, m.slot, why)) return false;
    out = m;
    return true;
}

bool from_command(const CommandRequest& r, ClosePluginEditor& out, std::string& why) {
    ClosePluginEditor m;
    if (!read(r, kCommandClosePluginEditor, m.track, m.slot, why)) return false;
    out = m;
    return true;
}

EngineEvent to_event(const PluginEditorState& m) {
    EngineEvent e;
    e.kind = EventKind::PluginEditor;
    e.track = static_cast<int32_t>(m.track);
    e.insert = m.slot;
    e.flags = static_cast<uint32_t>(m.state);
    e.text = m.state == PluginEditorStatus::Failed ? m.reason : std::string();
    return e;
}

bool from_event(const EngineEvent& e, PluginEditorState& out, std::string& why) {
    if (e.kind != EventKind::PluginEditor) {
        why = "not a plug-in editor event";
        return false;
    }
    if (e.flags > kLastPluginEditorStatus) {
        why = "an editor in state " + std::to_string(e.flags) + " this version does not define";
        return false;
    }
    if (e.track < 0) {
        why = "an editor event names no track";
        return false;
    }
    if (e.insert >= kInsertSlots) {
        why = "an editor event names slot " + std::to_string(e.insert) + ", past the last";
        return false;
    }
    PluginEditorState m;
    m.state = static_cast<PluginEditorStatus>(e.flags);
    if (m.state != PluginEditorStatus::Failed && !e.text.empty()) {
        why = "an editor event carries a reason on a state that is not Failed";
        return false;
    }
    m.track = static_cast<uint32_t>(e.track);
    m.slot = static_cast<uint32_t>(e.insert);
    m.reason = e.text;
    out = std::move(m);
    return true;
}

}  // namespace sonora::api
