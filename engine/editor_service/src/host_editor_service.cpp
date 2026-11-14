// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/editor_service/host_editor_service.h"

namespace sonora::editor_service {

namespace {
bool live(EditorState s) { return s == EditorState::Opening || s == EditorState::Open; }
}  // namespace

void HostEditorService::set(const Key& key, EditorStatus next) {
    EditorStatus& cur = editors_[key].status;
    if (cur == next) return;
    cur = std::move(next);
    if (listener_) listener_(key.first, key.second, cur);
}

EditorStatus HostEditorService::open(TrackIndex track, SlotIndex slot) {
    const Key key{track, slot};
    Entry& e = editors_[key];
    if (live(e.status.state)) return e.status;  // raise: no second request, no second window
    plugin::control::OpenEditor msg;
    msg.request = next_request_++;
    msg.instance = slot;  // parent_token stays empty: parentless toplevel
    e.request = msg.request;
    if (!link_.send_open(track, msg)) {
        set(key, EditorStatus{EditorState::Failed, "no plug-in host is running for this track"});
    } else {
        set(key, EditorStatus{EditorState::Opening, {}});
    }
    return editors_[key].status;
}

void HostEditorService::close_if_live(const Key& key) {
    auto it = editors_.find(key);
    if (it == editors_.end() || !live(it->second.status.state)) return;
    set(key, EditorStatus{});
}

void HostEditorService::close(TrackIndex track, SlotIndex slot) {
    auto it = editors_.find(Key{track, slot});
    if (it == editors_.end() || !live(it->second.status.state)) return;
    plugin::control::CloseEditor msg;
    msg.instance = slot;
    link_.send_close(track, msg);
    // The host's EditorClosed would only repeat this; the state is Closed now.
    set(it->first, EditorStatus{});
}

EditorStatus HostEditorService::status(TrackIndex track, SlotIndex slot) const {
    auto it = editors_.find(Key{track, slot});
    return it == editors_.end() ? EditorStatus{} : it->second.status;
}

void HostEditorService::on_editor_opened(TrackIndex track, const plugin::control::EditorOpened& m) {
    const Key key{track, m.instance};
    auto it = editors_.find(key);
    if (it == editors_.end() || it->second.status.state != EditorState::Opening ||
        it->second.request != m.request)
        return;
    set(key, EditorStatus{EditorState::Open, {}});
}

void HostEditorService::on_editor_failed(TrackIndex track, const plugin::control::EditorFailed& m) {
    const Key key{track, m.instance};
    auto it = editors_.find(key);
    if (it == editors_.end() || it->second.status.state != EditorState::Opening ||
        it->second.request != m.request)
        return;
    set(key, EditorStatus{EditorState::Failed, m.text});
}

void HostEditorService::on_editor_closed(TrackIndex track, const plugin::control::EditorClosed& m) {
    close_if_live(Key{track, m.instance});
}

void HostEditorService::on_host_stopped(TrackIndex track) {
    for (auto& entry : editors_)
        if (entry.first.first == track) close_if_live(entry.first);
}

void HostEditorService::on_insert_removed(TrackIndex track, SlotIndex slot) {
    close_if_live(Key{track, slot});
}

}  // namespace sonora::editor_service
