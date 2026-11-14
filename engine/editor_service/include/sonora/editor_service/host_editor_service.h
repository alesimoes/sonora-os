// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginEditorService over the host control channel (ADR-0088, ADR-0214).
// The channel is an interface so tests use a fake and the wire follow-up
// binds it to HostSupervisor. Control thread only; never the audio thread.
// An insert's instance number in its host is its slot index.
#pragma once

#include "sonora/editor_service/plugin_editor_service.h"
#include "sonora/plugin/host_control.h"

#include <cstdint>
#include <map>
#include <utility>

namespace sonora::editor_service {

class EditorHostLink {
public:
    virtual ~EditorHostLink() = default;
    // False when no host runs this track's chain; nothing was sent.
    virtual bool send_open(TrackIndex track, const plugin::control::OpenEditor& msg) = 0;
    virtual void send_close(TrackIndex track, const plugin::control::CloseEditor& msg) = 0;
};

class HostEditorService final : public PluginEditorService {
public:
    explicit HostEditorService(EditorHostLink& link) : link_(link) {}

    EditorStatus open(TrackIndex track, SlotIndex slot) override;
    void close(TrackIndex track, SlotIndex slot) override;
    EditorStatus status(TrackIndex track, SlotIndex slot) const override;
    void set_listener(EditorListener listener) override { listener_ = std::move(listener); }

    // Host events, fed by whoever pumps the supervisor.
    void on_editor_opened(TrackIndex track, const plugin::control::EditorOpened& m);
    void on_editor_failed(TrackIndex track, const plugin::control::EditorFailed& m);
    void on_editor_closed(TrackIndex track, const plugin::control::EditorClosed& m);
    // The host crashed, stopped or was replaced: every editor of the track is Closed.
    void on_host_stopped(TrackIndex track);
    // The insert left the chain: its editor is Closed.
    void on_insert_removed(TrackIndex track, SlotIndex slot);

private:
    using Key = std::pair<TrackIndex, SlotIndex>;
    struct Entry {
        EditorStatus status;
        std::uint32_t request = 0;
    };
    void set(const Key& key, EditorStatus next);
    void close_if_live(const Key& key);

    EditorHostLink& link_;
    std::map<Key, Entry> editors_;
    EditorListener listener_;
    std::uint32_t next_request_ = 1;
};

}  // namespace sonora::editor_service
