// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's plug-in editor service (ADR-0088): the UI names an insert by
// {track, slot}, the engine finds the host that runs it and asks it to open
// or close the plug-in's own editor. The UI never talks to a host.
//
// Control thread only; nothing here is on the audio thread. Values moved in
// the editor never pass through this interface: they reach the document by
// the existing ParamTouched / gesture path (contracts/plugin host_control.h).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace sonora::editor_service {

using TrackIndex = std::uint32_t;
using SlotIndex = std::uint32_t;

enum class EditorState : std::uint8_t {
    Closed = 0,
    Opening = 1,
    Open = 2,
    Failed = 3,  // reason holds the host's EditorFailed text, unchanged
};

struct EditorStatus {
    EditorState state = EditorState::Closed;
    std::string reason;  // empty unless Failed
};

inline bool operator==(const EditorStatus& a, const EditorStatus& b) {
    return a.state == b.state && a.reason == b.reason;
}
inline bool operator!=(const EditorStatus& a, const EditorStatus& b) { return !(a == b); }

// Called on every state change and only on a change; the follow-up wire
// turns each call into a PluginEditorState broadcast.
using EditorListener = std::function<void(TrackIndex, SlotIndex, const EditorStatus&)>;

class PluginEditorService {
public:
    virtual ~PluginEditorService() = default;

    // Opens the insert's editor, or raises it if it is Opening or Open: one
    // editor per insert, never a second window. A Failed editor is tried
    // again. Returns the state after the request.
    virtual EditorStatus open(TrackIndex track, SlotIndex slot) = 0;

    // Closes it. Closing an editor that is Closed or Failed is ignored.
    virtual void close(TrackIndex track, SlotIndex slot) = 0;

    virtual EditorStatus status(TrackIndex track, SlotIndex slot) const = 0;

    virtual void set_listener(EditorListener listener) = 0;
};

// In-memory implementation of the rules above, for tests and for the UI
// before the engine wiring lands. By default an open request goes straight
// to Open; script_failure() and hold_opening() make the next request for an
// insert fail or stay Opening until complete_open().
class FakePluginEditorService final : public PluginEditorService {
public:
    EditorStatus open(TrackIndex track, SlotIndex slot) override {
        const Key key{track, slot};
        EditorStatus& current = editors_[key];
        if (current.state == EditorState::Opening || current.state == EditorState::Open) {
            ++raises_;
            return current;
        }
        auto failure = failures_.find(key);
        if (failure != failures_.end()) {
            std::string reason = std::move(failure->second);
            failures_.erase(failure);
            set(key, EditorStatus{EditorState::Failed, std::move(reason)});
        } else if (held_.erase(key) > 0) {
            set(key, EditorStatus{EditorState::Opening, {}});
        } else {
            ++windows_created_;
            set(key, EditorStatus{EditorState::Open, {}});
        }
        return editors_[key];
    }

    void close(TrackIndex track, SlotIndex slot) override {
        auto it = editors_.find(Key{track, slot});
        if (it == editors_.end()) return;
        const EditorState s = it->second.state;
        if (s == EditorState::Opening || s == EditorState::Open) set(it->first, EditorStatus{});
    }

    EditorStatus status(TrackIndex track, SlotIndex slot) const override {
        auto it = editors_.find(Key{track, slot});
        return it == editors_.end() ? EditorStatus{} : it->second;
    }

    void set_listener(EditorListener listener) override { listener_ = std::move(listener); }

    // The next open() of this insert answers EditorFailed with `reason`.
    void script_failure(TrackIndex track, SlotIndex slot, std::string reason) {
        failures_[Key{track, slot}] = std::move(reason);
    }

    // The next open() of this insert stays Opening until complete_open().
    void hold_opening(TrackIndex track, SlotIndex slot) { held_[Key{track, slot}] = true; }

    void complete_open(TrackIndex track, SlotIndex slot) {
        auto it = editors_.find(Key{track, slot});
        if (it == editors_.end() || it->second.state != EditorState::Opening) return;
        ++windows_created_;
        set(it->first, EditorStatus{EditorState::Open, {}});
    }

    // The host running this track's chain stopped or crashed: every editor
    // it had, Opening or Open, reports Closed (ADR-0088 decision 5).
    void host_stopped(TrackIndex track) {
        for (auto& entry : editors_) {
            if (entry.first.first != track) continue;
            const EditorState s = entry.second.state;
            if (s == EditorState::Opening || s == EditorState::Open) set(entry.first, EditorStatus{});
        }
    }

    // The insert left the chain: its editor reports Closed.
    void insert_removed(TrackIndex track, SlotIndex slot) { close(track, slot); }

    int windows_created() const { return windows_created_; }
    int raises() const { return raises_; }

private:
    using Key = std::pair<TrackIndex, SlotIndex>;

    void set(const Key& key, EditorStatus next) {
        EditorStatus& current = editors_[key];
        if (current == next) return;
        current = std::move(next);
        if (listener_) listener_(key.first, key.second, current);
    }

    std::map<Key, EditorStatus> editors_;
    std::map<Key, std::string> failures_;
    std::map<Key, bool> held_;
    EditorListener listener_;
    int windows_created_ = 0;
    int raises_ = 0;
};

}  // namespace sonora::editor_service
