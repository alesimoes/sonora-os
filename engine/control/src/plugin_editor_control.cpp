// SPDX-License-Identifier: GPL-3.0-or-later
// The editor service over the hosts (plugin_editor_control.h, ADR-0088).
#include "sonora/plugin_editor_control.h"

#include "sonora/model/model.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_hosting.h"

namespace sonora {

namespace pc = plugin::control;
using editor_service::EditorState;
using editor_service::EditorStatus;

namespace {

bool live(EditorState s) { return s == EditorState::Opening || s == EditorState::Open; }

bool same(const EditorHostPort::Target& a, const EditorHostPort::Target& b) {
    return a.host == b.host && a.instance == b.instance;
}

}  // namespace

uint32_t PluginEditorControl::next_request() {
    const uint32_t r = request_;
    request_ = request_ == UINT32_MAX ? kFirstRequest : request_ + 1;
    return r;
}

EditorStatus PluginEditorControl::open(TrackIndex track, SlotIndex slot) {
    const Key key{track, slot};
    Editor& e = editors_[key];
    if (e.status.state == EditorState::Open) {
        // Decision 2: the host raises a window it already has and answers
        // EditorFailed, which on_host_message does not report. A raise that
        // cannot be sent changes nothing the operator sees.
        std::string ignored;
        send_open(e, key, false, ignored);
        return e.status;
    }
    // Opening: the window is on its way; a second request would be a
    // second window.
    if (e.status.state == EditorState::Opening) return e.status;
    std::string why;
    if (!send_open(e, key, true, why)) {
        set(key, EditorStatus{EditorState::Failed, why});
    } else {
        set(key, EditorStatus{EditorState::Opening, {}});
    }
    return editors_[key].status;
}

void PluginEditorControl::close(TrackIndex track, SlotIndex slot) {
    const auto it = editors_.find(Key{track, slot});
    if (it == editors_.end() || !live(it->second.status.state)) return;
    drop(it->first, it->second, true);
}

EditorStatus PluginEditorControl::status(TrackIndex track, SlotIndex slot) const {
    const auto it = editors_.find(Key{track, slot});
    return it == editors_.end() ? EditorStatus{} : it->second.status;
}

bool PluginEditorControl::send_open(Editor& e, const Key& key, bool owned, std::string& why) {
    EditorHostPort::Target t = e.target;
    if (owned && !hosts_.resolve(key.first, key.second, t, why)) return false;
    pc::OpenEditor m;
    m.request = next_request();
    m.instance = t.instance;
    // Decision 4: empty, a parentless toplevel the compositor places.
    std::string bytes;
    pc::encode(m, bytes);
    if (!hosts_.send(t.host, bytes, why)) return false;
    pending_[m.request] = Pending{key, owned, t};
    if (owned) {
        e.target = t;
        e.request = m.request;
    }
    return true;
}

void PluginEditorControl::send_close(const EditorHostPort::Target& target) {
    pc::CloseEditor m;
    m.instance = target.instance;
    std::string bytes, ignored;
    pc::encode(m, bytes);
    // A host that has gone took its window with it.
    hosts_.send(target.host, bytes, ignored);
}

void PluginEditorControl::set(const Key& key, EditorStatus next) {
    Editor& e = editors_[key];
    if (e.status == next) return;
    e.status = std::move(next);
    if (listener_) listener_(key.first, key.second, e.status);
}

void PluginEditorControl::drop(const Key& key, Editor& e, bool tell_host) {
    if (tell_host) send_close(e.target);
    // An OpenEditor still unanswered stays in pending_, no longer the
    // editor's: if its window opens after all, it is closed at once.
    e.request = 0;
    set(key, EditorStatus{});
}

void PluginEditorControl::on_host_message(EditorHostPort::HostKey host, const std::string& bytes) {
    pc::Type type{};
    if (!pc::peek_type(bytes, type)) return;
    std::string error;
    if (type == pc::Type::EditorClosed) {
        pc::EditorClosed m;
        if (!pc::decode(bytes, m, error)) return;
        // Only an Open editor: one still Opening is a new request, and this
        // is the close of the window before it (close, then open again).
        for (auto& [key, e] : editors_) {
            if (e.status.state == EditorState::Open && e.target.host == host && e.target.instance == m.instance)
                drop(key, e, false);
        }
        return;
    }
    uint32_t request = 0;
    bool opened = false;
    std::string text;
    if (type == pc::Type::EditorOpened) {
        pc::EditorOpened m;
        if (!pc::decode(bytes, m, error)) return;
        request = m.request;
        opened = true;
    } else if (type == pc::Type::EditorFailed) {
        pc::EditorFailed m;
        if (!pc::decode(bytes, m, error)) return;
        request = m.request;
        text = std::move(m.text);
    } else {
        return;
    }
    const auto p = pending_.find(request);
    if (p == pending_.end() || p->second.target.host != host) return;
    const Pending pending = p->second;
    pending_.erase(p);
    const auto it = editors_.find(pending.key);
    const bool current = pending.owned && it != editors_.end() && it->second.request == request &&
                         it->second.status.state == EditorState::Opening;
    if (current) {
        it->second.request = 0;
        if (opened) {
            set(pending.key, EditorStatus{EditorState::Open, {}});
        } else {
            // Decision 3: the host's reason, unchanged.
            set(pending.key, EditorStatus{EditorState::Failed, std::move(text)});
        }
        return;
    }
    // A raise refused (the window was there) or an open the operator closed
    // before its answer: nothing to report. A window that opened for one is
    // closed, unless it is the very window an Open editor already shows.
    if (!opened) return;
    const bool shown = it != editors_.end() && it->second.status.state == EditorState::Open &&
                       same(it->second.target, pending.target);
    if (!shown) send_close(pending.target);
}

void PluginEditorControl::reconcile() {
    for (auto& [key, e] : editors_) {
        if (!live(e.status.state)) continue;
        EditorHostPort::Target now;
        std::string why;
        if (hosts_.resolve(key.first, key.second, now, why) && same(now, e.target)) continue;
        // Decision 5: the host went (its window with it), or the insert is no
        // longer what that host runs at this slot; a host still running is
        // asked to close the window it would otherwise keep.
        drop(key, e, true);
    }
    // Requests to hosts no slot resolves to any more will never be answered.
    for (auto it = pending_.begin(); it != pending_.end();) {
        EditorHostPort::Target now;
        std::string why;
        const bool alive = hosts_.resolve(it->second.key.first, it->second.key.second, now, why) &&
                           same(now, it->second.target);
        it = alive ? std::next(it) : pending_.erase(it);
    }
}

void PluginEditorControl::session_opened() {
    for (auto& [key, e] : editors_) {
        e.request = 0;
        set(key, EditorStatus{});
    }
    editors_.clear();
    pending_.clear();
}

std::vector<PluginEditorControl::Entry> PluginEditorControl::standing() const {
    std::vector<Entry> out;
    for (const auto& [key, e] : editors_)
        if (e.status.state != EditorState::Closed) out.push_back(Entry{key.first, key.second, e.status});
    return out;
}

// ---- the server's port

namespace {

class HostingEditorPort final : public EditorHostPort {
public:
    HostingEditorPort(std::function<PluginHosting*()> hosting, std::function<const model::Snapshot&()> document)
        : hosting_(std::move(hosting)), document_(std::move(document)) {}

    bool resolve(uint32_t track, uint32_t slot, Target& out, std::string& why) const override {
        PluginHosting* h = hosting_();
        if (h == nullptr || !h->ok()) {
            why = "plug-in hosting is not running";
            return false;
        }
        const model::Snapshot& doc = document_();
        if (!doc.edit || track >= doc.edit->tracks.size()) {
            why = "there is no track " + std::to_string(track + 1);
            return false;
        }
        const model::TrackEdit& t = *doc.edit->tracks[track];
        const std::string where = t.name + ", slot " + std::string(1, char('A' + slot));
        for (const model::Insert& in : t.inserts) {
            if (in.slot != slot) continue;
            if (!in.active) {
                why = in.plugin.name + " (" + where + ") is inactive";
                return false;
            }
            const void* host = h->editor_host(t.id, in.id, out.instance, why);
            if (host == nullptr) {
                why = in.plugin.name + " (" + where + "): " + why;
                return false;
            }
            out.host = host;
            return true;
        }
        why = "there is no plug-in at " + where;
        return false;
    }

    bool send(HostKey host, const std::string& encoded, std::string& why) override {
        PluginHosting* h = hosting_();
        if (h == nullptr) {
            why = "plug-in hosting is not running";
            return false;
        }
        return h->send_to_editor_host(host, encoded, why);
    }

private:
    std::function<PluginHosting*()> hosting_;
    std::function<const model::Snapshot&()> document_;
};

}  // namespace

std::unique_ptr<EditorHostPort> make_hosting_editor_port(std::function<PluginHosting*()> hosting,
                                                         std::function<const model::Snapshot&()> document) {
    return std::make_unique<HostingEditorPort>(std::move(hosting), std::move(document));
}

}  // namespace sonora
