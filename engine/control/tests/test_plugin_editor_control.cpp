// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginEditorControl (ADR-0088) against a fake host port: the host_control
// messages it sends, and the states the host's answers, a crash and a chain
// change give. Then the wire routes on a CommandRegistry. No process, no
// socket, no clock.
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sonora/api/plugin_editor.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_editor_control.h"
#include "sonora/plugin_editor_routes.h"

using namespace sonora;
namespace pc = plugin::control;
using editor_service::EditorState;
using editor_service::EditorStatus;

namespace {

int failures = 0;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// Two hosts' identities; the fake never dereferences them either. Not
// const, so no linker folds them into one address.
int kHostA = 1;
int kHostB = 2;
const EditorHostPort::HostKey A = &kHostA;
const EditorHostPort::HostKey B = &kHostB;

struct Sent {
    EditorHostPort::HostKey host = nullptr;
    pc::Type type{};
    pc::OpenEditor open;
    pc::CloseEditor close;
};

// The inserts the hosts run, by {track, slot}; and what was sent to them.
class FakeHosts final : public EditorHostPort {
public:
    std::map<std::pair<uint32_t, uint32_t>, Target> running;
    std::vector<Sent> sent;
    bool refuse_sends = false;

    bool resolve(uint32_t track, uint32_t slot, Target& out, std::string& why) const override {
        const auto it = running.find({track, slot});
        if (it == running.end()) {
            why = "there is no plug-in at track " + std::to_string(track) + ", slot " + std::to_string(slot);
            return false;
        }
        out = it->second;
        return true;
    }

    bool send(HostKey host, const std::string& encoded, std::string& why) override {
        bool alive = false;
        for (const auto& [key, t] : running) alive = alive || t.host == host;
        if (!alive || refuse_sends) {
            why = "its plug-in host has stopped";
            return false;
        }
        Sent s;
        s.host = host;
        std::string error;
        if (!pc::peek_type(encoded, s.type)) return false;
        if (s.type == pc::Type::OpenEditor) pc::decode(encoded, s.open, error);
        if (s.type == pc::Type::CloseEditor) pc::decode(encoded, s.close, error);
        sent.push_back(s);
        return true;
    }

    // The host stopped or crashed: nothing resolves to it any more.
    void crash(HostKey host) {
        for (auto it = running.begin(); it != running.end();)
            it = it->second.host == host ? running.erase(it) : std::next(it);
    }

    const Sent& last() const { return sent.back(); }
};

std::string opened(uint32_t request, uint32_t instance) {
    pc::EditorOpened m;
    m.request = request;
    m.instance = instance;
    std::string b;
    pc::encode(m, b);
    return b;
}

std::string failed(uint32_t request, uint32_t instance, const std::string& text) {
    pc::EditorFailed m;
    m.request = request;
    m.instance = instance;
    m.text = text;
    std::string b;
    pc::encode(m, b);
    return b;
}

std::string closed(uint32_t instance, pc::EditorCloseReason reason) {
    pc::EditorClosed m;
    m.instance = instance;
    m.reason = reason;
    std::string b;
    pc::encode(m, b);
    return b;
}

struct Rig {
    FakeHosts hosts;
    PluginEditorControl editors{hosts};
    std::vector<std::pair<std::pair<uint32_t, uint32_t>, EditorStatus>> heard;
    Rig() {
        hosts.running[{1, 2}] = EditorHostPort::Target{A, 1};
        hosts.running[{1, 0}] = EditorHostPort::Target{A, 0};
        hosts.running[{3, 4}] = EditorHostPort::Target{B, 0};
        editors.set_listener([this](uint32_t t, uint32_t s, const EditorStatus& st) { heard.push_back({{t, s}, st}); });
    }
    EditorState state(uint32_t t, uint32_t s) const { return editors.status(t, s).state; }
};

void open_reaches_the_host_and_opens() {
    Rig r;
    const EditorStatus s = r.editors.open(1, 2);
    CHECK(s.state == EditorState::Opening);
    CHECK(r.hosts.sent.size() == 1);
    CHECK(r.hosts.last().host == A && r.hosts.last().type == pc::Type::OpenEditor);
    CHECK(r.hosts.last().open.instance == 1);
    // Decision 4: a parentless toplevel.
    CHECK(r.hosts.last().open.parent_token.empty());
    CHECK(r.hosts.last().open.request >= PluginEditorControl::kFirstRequest);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 1));
    CHECK(r.state(1, 2) == EditorState::Open);
    CHECK(r.heard.size() == 2);
    CHECK(r.heard[0].second.state == EditorState::Opening && r.heard[1].second.state == EditorState::Open);
    CHECK(r.heard[1].first == std::make_pair(1u, 2u));
    // Another insert's state is untouched.
    CHECK(r.state(1, 0) == EditorState::Closed);
    // A reply to no request of ours, or from another host, changes nothing.
    r.editors.on_host_message(B, opened(r.hosts.last().open.request, 1));
    r.editors.on_host_message(A, failed(12345, 1, "stray"));
    CHECK(r.state(1, 2) == EditorState::Open && r.heard.size() == 2);
}

void double_open_raises_never_duplicates() {
    Rig r;
    r.editors.open(1, 2);
    // Opening: the window is on its way; nothing more is sent.
    CHECK(r.editors.open(1, 2).state == EditorState::Opening);
    CHECK(r.hosts.sent.size() == 1);
    r.editors.on_host_message(A, opened(r.hosts.sent[0].open.request, 1));
    // Open: the host is asked again, which raises the window and answers
    // EditorFailed (host_control.h); that is not a failure (decision 2).
    CHECK(r.editors.open(1, 2).state == EditorState::Open);
    CHECK(r.hosts.sent.size() == 2);
    CHECK(r.hosts.last().type == pc::Type::OpenEditor && r.hosts.last().open.instance == 1);
    CHECK(r.hosts.last().open.request != r.hosts.sent[0].open.request);
    const size_t heard = r.heard.size();
    r.editors.on_host_message(A, failed(r.hosts.last().open.request, 1, "the editor is already open"));
    CHECK(r.state(1, 2) == EditorState::Open);
    CHECK(r.editors.status(1, 2).reason.empty());
    CHECK(r.heard.size() == heard);
}

void failed_open_carries_the_hosts_reason() {
    Rig r;
    r.editors.open(1, 2);
    const std::string why = "no display: DISPLAY is not set";
    r.editors.on_host_message(A, failed(r.hosts.last().open.request, 1, why));
    CHECK(r.state(1, 2) == EditorState::Failed);
    CHECK(r.editors.status(1, 2).reason == why);
    CHECK(r.heard.back().second.reason == why);
    // Not sticky: a new request tries again.
    CHECK(r.editors.open(1, 2).state == EditorState::Opening);
    CHECK(r.hosts.sent.size() == 2);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 1));
    CHECK(r.state(1, 2) == EditorState::Open && r.editors.status(1, 2).reason.empty());
    // Closing a Failed editor is ignored.
    Rig q;
    q.editors.open(1, 2);
    q.editors.on_host_message(A, failed(q.hosts.last().open.request, 1, why));
    q.editors.close(1, 2);
    CHECK(q.state(1, 2) == EditorState::Failed && q.hosts.sent.size() == 1);
}

void nothing_to_open_fails_with_why() {
    Rig r;
    const EditorStatus s = r.editors.open(5, 1);
    CHECK(s.state == EditorState::Failed);
    CHECK(s.reason == "there is no plug-in at track 5, slot 1");
    CHECK(r.hosts.sent.empty());
    r.hosts.refuse_sends = true;
    CHECK(r.editors.open(1, 2).state == EditorState::Failed);
    CHECK(r.editors.status(1, 2).reason == "its plug-in host has stopped");
}

void host_crash_closes_its_editors() {
    Rig r;
    r.editors.open(1, 2);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 1));
    r.editors.open(1, 0);  // still Opening when the host goes
    const uint32_t late = r.hosts.last().open.request;
    r.editors.open(3, 4);
    r.editors.on_host_message(B, opened(r.hosts.last().open.request, 0));
    r.hosts.crash(A);
    // No EditorClosed comes from a host that crashed (decision 5).
    r.editors.reconcile();
    CHECK(r.state(1, 2) == EditorState::Closed);
    CHECK(r.state(1, 0) == EditorState::Closed);
    CHECK(r.state(3, 4) == EditorState::Open);
    CHECK(r.heard.back().second.state == EditorState::Closed);
    // A reply the dead host never sent cannot reopen it; nor can a second pass.
    r.editors.on_host_message(A, opened(late, 0));
    r.editors.reconcile();
    CHECK(r.state(1, 0) == EditorState::Closed);
    CHECK(r.editors.standing().size() == 1);
    // A restarted host runs the insert again: the operator opens it anew.
    static int restarted = 3;
    r.hosts.running[{1, 2}] = EditorHostPort::Target{&restarted, 1};
    CHECK(r.editors.open(1, 2).state == EditorState::Opening);
    CHECK(r.hosts.last().host == &restarted);
}

void chain_removed_closes_and_tells_the_host() {
    Rig r;
    r.editors.open(1, 2);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 1));
    // The insert left the chain; its host still runs the others for now.
    r.hosts.running.erase({1, 2});
    const size_t before = r.hosts.sent.size();
    r.editors.reconcile();
    CHECK(r.state(1, 2) == EditorState::Closed);
    CHECK(r.hosts.sent.size() == before + 1);
    CHECK(r.hosts.last().type == pc::Type::CloseEditor && r.hosts.last().host == A);
    CHECK(r.hosts.last().close.instance == 1);
    // The chain was built again on another host: the old window is gone.
    r.editors.open(1, 0);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 0));
    r.hosts.running[{1, 0}] = EditorHostPort::Target{B, 2};
    r.editors.reconcile();
    CHECK(r.state(1, 0) == EditorState::Closed);
    // An unchanged chain keeps its editor.
    r.editors.open(3, 4);
    r.editors.on_host_message(B, opened(r.hosts.last().open.request, 0));
    r.editors.reconcile();
    CHECK(r.state(3, 4) == EditorState::Open);
}

void closes_by_request_and_by_operator() {
    Rig r;
    r.editors.open(1, 2);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 1));
    r.editors.close(1, 2);
    CHECK(r.state(1, 2) == EditorState::Closed);
    CHECK(r.hosts.last().type == pc::Type::CloseEditor && r.hosts.last().close.instance == 1);
    // Closed again: ignored, nothing sent.
    const size_t sent = r.hosts.sent.size();
    r.editors.close(1, 2);
    CHECK(r.hosts.sent.size() == sent);
    // Reopened before the host's EditorClosed{Requested} for the last window
    // arrives: that close is not this window's.
    r.editors.open(1, 2);
    const uint32_t req = r.hosts.last().open.request;
    r.editors.on_host_message(A, closed(1, pc::EditorCloseReason::Requested));
    CHECK(r.state(1, 2) == EditorState::Opening);
    r.editors.on_host_message(A, opened(req, 1));
    CHECK(r.state(1, 2) == EditorState::Open);
    // The operator closes the window; another host's close is not ours.
    r.editors.on_host_message(B, closed(1, pc::EditorCloseReason::ByOperator));
    CHECK(r.state(1, 2) == EditorState::Open);
    r.editors.on_host_message(A, closed(1, pc::EditorCloseReason::ByOperator));
    CHECK(r.state(1, 2) == EditorState::Closed);
}

void closed_while_opening_closes_the_late_window() {
    Rig r;
    r.editors.open(1, 2);
    const uint32_t req = r.hosts.last().open.request;
    r.editors.close(1, 2);
    CHECK(r.state(1, 2) == EditorState::Closed);
    const size_t sent = r.hosts.sent.size();
    r.editors.on_host_message(A, opened(req, 1));
    CHECK(r.state(1, 2) == EditorState::Closed);
    CHECK(r.hosts.sent.size() == sent + 1);
    CHECK(r.hosts.last().type == pc::Type::CloseEditor && r.hosts.last().close.instance == 1);
}

void a_session_opened_closes_every_editor() {
    Rig r;
    r.editors.open(1, 2);
    r.editors.on_host_message(A, opened(r.hosts.last().open.request, 1));
    r.editors.open(3, 4);
    r.editors.on_host_message(B, failed(r.hosts.last().open.request, 0, "LV2 UIs are not opened"));
    CHECK(r.editors.standing().size() == 2);
    r.editors.session_opened();
    CHECK(r.state(1, 2) == EditorState::Closed);
    CHECK(r.state(3, 4) == EditorState::Closed);
    CHECK(r.editors.standing().empty());
    CHECK(r.heard.back().second.state == EditorState::Closed);
}

void routes_parse_and_run_the_service() {
    editor_service::FakePluginEditorService fake;
    CommandRegistry registry;
    install_plugin_editor_routes(registry, fake);
    CHECK(registry.implemented(api::kCommandOpenPluginEditor));
    CHECK(registry.implemented(api::kCommandClosePluginEditor));
    CommandContext ctx;
    ctx.target = 1;
    ctx.argument = "2";
    std::string why;
    CHECK(registry.dispatch(api::kCommandOpenPluginEditor, ctx, why) == DispatchResult::Ok);
    CHECK(fake.status(1, 2).state == EditorState::Open);
    // The reply is Ok once taken; a failure is the event's to carry.
    fake.script_failure(1, 3, "no display");
    ctx.argument = "3";
    CHECK(registry.dispatch(api::kCommandOpenPluginEditor, ctx, why) == DispatchResult::Ok);
    CHECK(fake.status(1, 3).state == EditorState::Failed);
    ctx.argument = "2";
    CHECK(registry.dispatch(api::kCommandClosePluginEditor, ctx, why) == DispatchResult::Ok);
    CHECK(fake.status(1, 2).state == EditorState::Closed);
    // What the wire refuses fails with why and reaches no editor.
    ctx.argument = "x";
    why.clear();
    CHECK(registry.dispatch(api::kCommandOpenPluginEditor, ctx, why) == DispatchResult::Failed);
    CHECK(!why.empty());
    ctx.target = -1;
    ctx.argument = "1";
    why.clear();
    CHECK(registry.dispatch(api::kCommandClosePluginEditor, ctx, why) == DispatchResult::Failed);
    CHECK(!why.empty());
    CHECK(fake.windows_created() == 1);
}

void the_event_carries_the_state() {
    api::PluginEditorState s;
    std::string why;
    CHECK(api::from_event(plugin_editor_event(2, 7, EditorStatus{EditorState::Failed, "no display"}), s, why));
    CHECK(s.track == 2 && s.slot == 7 && s.state == api::PluginEditorStatus::Failed && s.reason == "no display");
    CHECK(api::from_event(plugin_editor_event(0, 0, EditorStatus{EditorState::Open, {}}), s, why));
    CHECK(s.state == api::PluginEditorStatus::Open && s.reason.empty());
    CHECK(api::from_event(plugin_editor_event(4, 9, EditorStatus{}), s, why));
    CHECK(s.state == api::PluginEditorStatus::Closed && s.track == 4 && s.slot == 9);
}

}  // namespace

int main() {
    open_reaches_the_host_and_opens();
    double_open_raises_never_duplicates();
    failed_open_carries_the_hosts_reason();
    nothing_to_open_fails_with_why();
    host_crash_closes_its_editors();
    chain_removed_closes_and_tells_the_host();
    closes_by_request_and_by_operator();
    closed_while_opening_closes_the_late_window();
    a_session_opened_closes_every_editor();
    routes_parse_and_run_the_service();
    the_event_carries_the_state();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("plugin_editor_control: ok\n");
    return 0;
}
