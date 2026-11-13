// SPDX-License-Identifier: GPL-3.0-or-later
// FakeEngine's plug-in editors (ADR-0088, protocol 70): the window states
// engine/editor_service's rules give, with no host and no window. An open
// reaches Opening and then, unless held, Open or the scripted failure.
#include <utility>

#include "sonora/api/fake_engine.h"
#include "sonora/model/model.h"
#include "sonora/model/replication.h"

namespace sonora::api::testing {

bool FakeEngine::plugin_editor(const CommandRequest& r, std::string& why) {
    const bool open = r.command == kCommandOpenPluginEditor;
    uint32_t track = 0;
    uint32_t slot = 0;
    if (open) {
        OpenPluginEditor m;
        if (!from_command(r, m, why)) return false;
        track = m.track;
        slot = m.slot;
    } else {
        ClosePluginEditor m;
        if (!from_command(r, m, why)) return false;
        track = m.track;
        slot = m.slot;
    }
    const model::Snapshot& s = document_->current();
    if (track >= s.edit->tracks.size()) {
        why = "there is no track " + std::to_string(track + 1);
        return false;
    }
    bool found = false;
    for (const model::Insert& in : s.edit->tracks[track]->inserts) found = found || in.slot == slot;
    if (!found) {
        why = "track " + std::to_string(track + 1) + " has no insert in slot " +
              std::to_string(slot);
        return false;
    }
    const PluginEditorStatus now = editor_status(track, slot);
    const bool live = now == PluginEditorStatus::Opening || now == PluginEditorStatus::Open;
    if (!open) {
        if (live) set_editor(track, slot, PluginEditorStatus::Closed, "");
        return true;
    }
    // One editor per insert: a second open raises the window, no new state.
    if (live) return true;
    set_editor(track, slot, PluginEditorStatus::Opening, "");
    if (!hold_editor_opening_) complete_editor_open(track, slot);
    return true;
}

void FakeEngine::complete_editor_open(uint32_t track, uint32_t slot) {
    if (editor_status(track, slot) != PluginEditorStatus::Opening) return;
    const auto failure = editor_failures_.find({track, slot});
    if (failure != editor_failures_.end()) {
        set_editor(track, slot, PluginEditorStatus::Failed, failure->second);
    } else {
        set_editor(track, slot, PluginEditorStatus::Open, "");
    }
}

void FakeEngine::script_editor_failure(uint32_t track, uint32_t slot, std::string reason) {
    if (reason.empty()) {
        editor_failures_.erase({track, slot});
    } else {
        editor_failures_[{track, slot}] = std::move(reason);
    }
}

void FakeEngine::editor_host_stopped(uint32_t track) {
    std::vector<std::pair<uint32_t, uint32_t>> live;
    for (const auto& e : editors_) {
        if (e.first.first == track) live.push_back(e.first);
    }
    for (const auto& k : live) {
        const PluginEditorStatus st = editor_status(k.first, k.second);
        if (st == PluginEditorStatus::Opening || st == PluginEditorStatus::Open) {
            set_editor(k.first, k.second, PluginEditorStatus::Closed, "");
        }
    }
}

PluginEditorStatus FakeEngine::editor_status(uint32_t track, uint32_t slot) const {
    const auto it = editors_.find({track, slot});
    return it == editors_.end() ? PluginEditorStatus::Closed : it->second.state;
}

void FakeEngine::set_editor(uint32_t track, uint32_t slot, PluginEditorStatus state,
                            std::string reason) {
    PluginEditorState m;
    m.track = track;
    m.slot = slot;
    m.state = state;
    m.reason = std::move(reason);
    post_event(to_event(m));
    if (state == PluginEditorStatus::Closed) {
        editors_.erase({track, slot});
    } else {
        editors_[{track, slot}] = std::move(m);
    }
}

void FakeEngine::announce_editors() {
    for (const auto& e : editors_) post_event(to_event(e.second));
}

void FakeEngine::close_all_editors() {
    std::vector<std::pair<uint32_t, uint32_t>> keys;
    for (const auto& e : editors_) keys.push_back(e.first);
    // Failed ones too: their notice names an insert of the last session.
    for (const auto& k : keys) set_editor(k.first, k.second, PluginEditorStatus::Closed, "");
}

}  // namespace sonora::api::testing
