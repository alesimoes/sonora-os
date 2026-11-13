// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginEditorService over the plug-in hosts (ADR-0088): the operator names
// an insert by {track index, slot}; this finds the host running it, sends
// OpenEditor / CloseEditor (contracts/plugin host_control.h) and turns the
// host's EditorOpened, EditorFailed and EditorClosed into the four states.
//
// Control thread only; nothing here touches the audio thread. The hosts are
// reached through EditorHostPort, so the rules are tested against a fake
// host and the server plugs in PluginHosting (make_hosting_editor_port).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sonora/editor_service/plugin_editor_service.h"

namespace sonora {

class PluginHosting;
namespace model {
struct Snapshot;
}

// What the editor service needs from whoever runs the hosts.
class EditorHostPort {
public:
    // A running host's identity. The service compares it and hands it back
    // to send(); it never dereferences it, so a host that has gone leaves a
    // stale key behind, never a dangling pointer.
    using HostKey = const void*;

    struct Target {
        HostKey host = nullptr;
        uint32_t instance = 0;  // the insert's index in the host's chain
    };

    virtual ~EditorHostPort() = default;
    // The host running the insert at {track index, slot} now and its
    // instance. False, `why` said to the operator, when there is none: no
    // track or insert there, the insert inactive, refused or missing, its
    // chain starting, restarting or down, or plug-in hosting off.
    virtual bool resolve(uint32_t track, uint32_t slot, Target& out, std::string& why) const = 0;
    // Queues an encoded host_control message for `host` if it is still a
    // running host; false, `why` set, when it is not or will not take it.
    virtual bool send(HostKey host, const std::string& encoded, std::string& why) = 0;
};

class PluginEditorControl final : public editor_service::PluginEditorService {
public:
    using TrackIndex = editor_service::TrackIndex;
    using SlotIndex = editor_service::SlotIndex;
    using EditorStatus = editor_service::EditorStatus;

    explicit PluginEditorControl(EditorHostPort& hosts) : hosts_(hosts) {}

    // ---- PluginEditorService (ADR-0088 decisions 2, 3, 5)
    EditorStatus open(TrackIndex track, SlotIndex slot) override;
    void close(TrackIndex track, SlotIndex slot) override;
    EditorStatus status(TrackIndex track, SlotIndex slot) const override;
    void set_listener(editor_service::EditorListener listener) override { listener_ = std::move(listener); }

    // A checked host message (HostProcess::take_messages) from `host`.
    // EditorOpened, EditorFailed and EditorClosed move the state; anything
    // else, or a reply to no request of ours, is ignored.
    void on_host_message(EditorHostPort::HostKey host, const std::string& bytes);
    // Every control-loop turn, after the hosts were polled: an editor whose
    // insert no longer resolves to the host it was opened on (the host
    // crashed, stopped or was replaced, the insert left the chain, hosting
    // went away) reports Closed. The host's own EditorClosed may never come.
    void reconcile();
    // A session opened: every editor of the last one reports Closed, Failed
    // included, so no notice outlives the insert it names (the amendment of
    // 2026-11-13). No message is sent: those hosts are being let go.
    void session_opened();

    // Every editor that is not Closed, for a client that attaches later.
    struct Entry {
        TrackIndex track = 0;
        SlotIndex slot = 0;
        EditorStatus status;
    };
    std::vector<Entry> standing() const;

    // OpenEditor's request ids are drawn from here up, so they never meet
    // the GetState ids PluginHosting counts up from 1 on the same host (a
    // host refuses a request id already outstanding).
    static constexpr uint32_t kFirstRequest = 0xE0000000u;

private:
    using Key = std::pair<TrackIndex, SlotIndex>;
    struct Editor {
        EditorStatus status;
        EditorHostPort::Target target;  // valid while Opening or Open
        uint32_t request = 0;           // the OpenEditor awaiting a reply, 0: none
    };
    // An OpenEditor in flight: whose it is, or (key unset) a raise or an
    // open closed before its answer, whose window is closed if it opens.
    struct Pending {
        Key key{};
        bool owned = false;
        EditorHostPort::Target target;
    };

    uint32_t next_request();
    bool send_open(Editor& e, const Key& key, bool owned, std::string& why);
    void send_close(const EditorHostPort::Target& target);
    void set(const Key& key, EditorStatus next);
    // Closed, its request (if any) left to close the window should it open.
    void drop(const Key& key, Editor& e, bool tell_host);

    EditorHostPort& hosts_;
    editor_service::EditorListener listener_;
    std::map<Key, Editor> editors_;
    std::map<uint32_t, Pending> pending_;
    uint32_t request_ = kFirstRequest;
};

// The server's port: PluginHosting's running hosts, the insert found by
// slot in the current document. Both are read through the callables at each
// call, since the server rebuilds hosting at every backend activation.
std::unique_ptr<EditorHostPort> make_hosting_editor_port(std::function<PluginHosting*()> hosting,
                                                         std::function<const model::Snapshot&()> document);

}  // namespace sonora
