// SPDX-License-Identifier: GPL-3.0-or-later
// Plug-in editors on the wire (ADR-0088, protocol 70): OpenPluginEditor,
// ClosePluginEditor and PluginEditorState cross the wire codec and come back
// equal; what a peer must not read is refused with a reason; and FakeEngine
// follows the ADR's rules: one editor per insert, Failed with the host's
// words and not sticky, Closed when the host stops or a session opens.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/fake_engine.h"
#include "sonora/api/plugin_editor.h"
#include "sonora/api/state_block.h"
#include "sonora/api/wire.h"
#include "sonora/model/model.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

using namespace sonora::api;
using sonora::api::testing::FakeEngine;
namespace model = sonora::model;

bool mentions(const std::string& text, const char* what) {
    return text.find(what) != std::string::npos;
}

// Through the bytes a socket carries, not just the struct mapping.
CommandRequest over_wire(const CommandRequest& r) {
    std::string bytes;
    wire::encode(r, bytes);
    CommandRequest back;
    std::string error;
    CHECK(wire::decode(bytes, back, error));
    return back;
}

EngineEvent over_wire(const EngineEvent& e) {
    std::string bytes;
    wire::encode(e, bytes);
    EngineEvent back;
    std::string error;
    CHECK(wire::decode(bytes, back, error));
    return back;
}

void open_round_trips() {
    OpenPluginEditor m;
    m.track = 17;
    m.slot = kInsertSlots - 1;
    const CommandRequest r = over_wire(to_command(m));
    CHECK(r.command == "plugin.editor_open");
    OpenPluginEditor back;
    std::string why;
    CHECK(from_command(r, back, why));
    CHECK(back.track == 17 && back.slot == kInsertSlots - 1);
    // A close is not read as an open.
    ClosePluginEditor wrong;
    CHECK(!from_command(r, wrong, why) && mentions(why, "plugin.editor_close"));
}

void close_round_trips() {
    ClosePluginEditor m;
    m.track = 0;
    m.slot = 3;
    const CommandRequest r = over_wire(to_command(m));
    CHECK(r.command == "plugin.editor_close");
    ClosePluginEditor back;
    std::string why;
    CHECK(from_command(r, back, why));
    CHECK(back.track == 0 && back.slot == 3);
}

void state_round_trips() {
    for (uint32_t s = 0; s <= kLastPluginEditorStatus; ++s) {
        PluginEditorState m;
        m.track = 4;
        m.slot = 2;
        m.state = static_cast<PluginEditorStatus>(s);
        if (m.state == PluginEditorStatus::Failed) m.reason = "no DISPLAY; the editor cannot open";
        const EngineEvent e = over_wire(to_event(m));
        CHECK(e.kind == EventKind::PluginEditor);
        PluginEditorState back;
        std::string why;
        CHECK(from_event(e, back, why));
        CHECK(back.track == 4 && back.slot == 2 && back.state == m.state);
        CHECK(back.reason == m.reason);
    }
    // A reason is kept only where the UI shows it.
    PluginEditorState open;
    open.state = PluginEditorStatus::Open;
    open.reason = "stale";
    CHECK(to_event(open).text.empty());
}

void commands_refused() {
    std::string why;
    OpenPluginEditor m;
    CommandRequest r = to_command(OpenPluginEditor{});
    r.target = -1;
    CHECK(!from_command(r, m, why) && mentions(why, "needs a track"));
    for (const char* arg : {"", "slot=", "slot=x", "slot=-1", "slot=+1", "slot=123", "1", "slot=1 "}) {
        r = to_command(OpenPluginEditor{});
        r.argument = arg;
        why.clear();
        CHECK(!from_command(r, m, why) && mentions(why, "slot="));
    }
    r.argument = "slot=" + std::to_string(kInsertSlots);
    CHECK(!from_command(r, m, why) && mentions(why, "past the last insert slot"));
}

void events_refused() {
    std::string why;
    PluginEditorState m;
    EngineEvent e = to_event(PluginEditorState{});
    e.flags = kLastPluginEditorStatus + 1;
    CHECK(!from_event(e, m, why) && mentions(why, "does not define"));
    // The codec refuses the state too, before any client reads it.
    std::string bytes, error;
    wire::encode(e, bytes);
    EngineEvent decoded;
    CHECK(!wire::decode(bytes, decoded, error) && mentions(error, "editor"));
    e = to_event(PluginEditorState{});
    e.track = -1;
    CHECK(!from_event(e, m, why) && mentions(why, "no track"));
    e = to_event(PluginEditorState{});
    e.insert = kInsertSlots;
    CHECK(!from_event(e, m, why) && mentions(why, "slot"));
    e = to_event(PluginEditorState{});
    e.text = "a reason on Closed";
    CHECK(!from_event(e, m, why) && mentions(why, "not Failed"));
    e.kind = EventKind::Render;
    CHECK(!from_event(e, m, why));
}

// --- FakeEngine -----------------------------------------------------------

struct Rig {
    explicit Rig(bool editors) {
        FakeEngine::Config cfg;
        cfg.tracks = 2;
        cfg.plugin_editors = editors;
        engine = std::make_unique<FakeEngine>(cfg);
        std::string error;
        CHECK(engine->connect(error));
        engine->poll(in);
    }
    void poll() {
        in.clear();
        engine->poll(in);
    }
    CommandReply reply_to(uint64_t seq) {
        poll();
        for (const CommandReply& c : in.replies) {
            if (c.seq == seq) return c;
        }
        return CommandReply{seq, DispatchResult::UnknownCommand, "no reply"};
    }
    CommandReply open(uint32_t track, uint32_t slot) {
        return reply_to(engine->send(to_command(OpenPluginEditor{track, slot})));
    }
    CommandReply close(uint32_t track, uint32_t slot) {
        return reply_to(engine->send(to_command(ClosePluginEditor{track, slot})));
    }
    // The editor states this poll carried, in order.
    std::vector<PluginEditorState> states() const {
        std::vector<PluginEditorState> out;
        for (const EngineEvent& e : in.events) {
            if (e.kind != EventKind::PluginEditor) continue;
            PluginEditorState m;
            std::string why;
            CHECK(from_event(e, m, why));
            out.push_back(m);
        }
        return out;
    }
    // A mono insert on track 0, slot `slot`.
    void add_insert(uint8_t slot) {
        const model::Snapshot& doc = engine->document();
        const PluginInfo& p = testing::fake_catalog()[0];
        model::AddInsert add;
        add.track = doc.edit->tracks[0]->id;
        add.insert = doc.next_id;
        add.slot = slot;
        add.plugin = model::PluginRef{static_cast<model::PluginFormat>(p.format), p.id, p.name,
                                      p.version};
        ModelRequest m;
        m.action = ModelAction::Apply;
        std::string error;
        CHECK(model::encode_operation(add, m.op, error));
        CHECK(reply_to(engine->submit(m)).result == DispatchResult::Ok);
    }
    std::unique_ptr<FakeEngine> engine;
    Inbox in;
};

bool announces(const Hello& h, const char* id) {
    for (const std::string& c : h.implemented_commands) {
        if (c == id) return true;
    }
    return false;
}

void fake_off_by_default() {
    Rig plain(false);
    CHECK(!announces(plain.engine->hello(), kCommandOpenPluginEditor));
    CHECK(plain.open(0, 0).result == DispatchResult::NotImplemented);
}

void fake_opens_once_and_closes() {
    Rig rig(true);
    CHECK(announces(rig.engine->hello(), kCommandOpenPluginEditor));
    CHECK(announces(rig.engine->hello(), kCommandClosePluginEditor));
    CommandReply r = rig.open(0, 0);
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "no insert in slot 0"));
    r = rig.open(5, 0);
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "no track 6"));
    rig.add_insert(0);

    CHECK(rig.open(0, 0).result == DispatchResult::Ok);
    std::vector<PluginEditorState> s = rig.states();
    CHECK(s.size() == 2 && s[0].state == PluginEditorStatus::Opening &&
          s[1].state == PluginEditorStatus::Open && s[1].track == 0 && s[1].slot == 0);
    // A second open raises the window: no second editor, no state change.
    CHECK(rig.open(0, 0).result == DispatchResult::Ok);
    CHECK(rig.states().empty());

    // A client that attaches later is sent the editor that stands.
    std::string error;
    CHECK(rig.engine->connect(error));
    rig.poll();
    s = rig.states();
    CHECK(s.size() == 1 && s[0].state == PluginEditorStatus::Open);

    CHECK(rig.close(0, 0).result == DispatchResult::Ok);
    s = rig.states();
    CHECK(s.size() == 1 && s[0].state == PluginEditorStatus::Closed);
    // Closing a Closed editor is ignored.
    CHECK(rig.close(0, 0).result == DispatchResult::Ok);
    CHECK(rig.states().empty());
}

void fake_failure_is_not_sticky() {
    Rig rig(true);
    rig.add_insert(1);
    rig.engine->script_editor_failure(0, 1, "the host was built without X11");
    CHECK(rig.open(0, 1).result == DispatchResult::Ok);
    std::vector<PluginEditorState> s = rig.states();
    CHECK(s.size() == 2 && s[1].state == PluginEditorStatus::Failed &&
          s[1].reason == "the host was built without X11");
    // Closing a Failed editor is ignored; a new open tries again.
    CHECK(rig.close(0, 1).result == DispatchResult::Ok);
    CHECK(rig.states().empty());
    rig.engine->script_editor_failure(0, 1, "");
    CHECK(rig.open(0, 1).result == DispatchResult::Ok);
    s = rig.states();
    CHECK(s.size() == 2 && s[1].state == PluginEditorStatus::Open);
}

void fake_closes_with_the_host() {
    Rig rig(true);
    rig.add_insert(0);
    rig.engine->hold_editor_opening(true);
    CHECK(rig.open(0, 0).result == DispatchResult::Ok);
    CHECK(rig.engine->editor_status(0, 0) == PluginEditorStatus::Opening);
    // Opening, too, ends Closed when its host stops.
    rig.engine->editor_host_stopped(0);
    rig.poll();
    std::vector<PluginEditorState> s = rig.states();
    CHECK(!s.empty() && s.back().state == PluginEditorStatus::Closed);
    CHECK(rig.engine->editor_status(0, 0) == PluginEditorStatus::Closed);
}

// Per-session: opening a session closes every editor of the last one, so
// no notice outlives the insert it names.
void fake_session_open_closes_editors() {
    Rig rig(true);
    rig.add_insert(0);
    rig.add_insert(1);
    rig.engine->script_editor_failure(0, 1, "no DISPLAY");
    CHECK(rig.open(0, 0).result == DispatchResult::Ok);
    CHECK(rig.open(0, 1).result == DispatchResult::Ok);
    std::string bytes, error;
    CHECK(model::serialize(rig.engine->document(), model::WriteInfo{"test", 1}, bytes, error));
    ModelRequest load;
    load.action = ModelAction::Load;
    load.op = wire::encode_load("", bytes);
    CHECK(rig.reply_to(rig.engine->submit(load)).result == DispatchResult::Ok);
    const std::vector<PluginEditorState> s = rig.states();
    CHECK(s.size() == 2);
    for (const PluginEditorState& m : s) CHECK(m.state == PluginEditorStatus::Closed);
    CHECK(rig.engine->editor_status(0, 0) == PluginEditorStatus::Closed);
    CHECK(rig.engine->editor_status(0, 1) == PluginEditorStatus::Closed);
}

}  // namespace

int main() {
    open_round_trips();
    close_round_trips();
    state_round_trips();
    commands_refused();
    events_refused();
    fake_off_by_default();
    fake_opens_once_and_closes();
    fake_failure_is_not_sticky();
    fake_closes_with_the_host();
    fake_session_open_closes_editors();
    if (failures != 0) {
        std::fprintf(stderr, "api_plugin_editor_wire: %d failed\n", failures);
        return 1;
    }
    std::puts("api_plugin_editor_wire: ok");
    return 0;
}
