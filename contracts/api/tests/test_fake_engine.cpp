// SPDX-License-Identifier: GPL-3.0-or-later
//
// FakeEngine against the control API's conformance suite, the same suite
// engine/control runs on InProcessEngine, plus what only a fake can be
// asked to do: be deterministic, script an event, die on cue.
#include "sonora/api/fake_engine.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/conformance.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"

using namespace sonora::api;
using sonora::DispatchResult;
using sonora::api::testing::FakeEngine;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

class FakeRig final : public conformance::Rig {
public:
    const char* name() const override { return "FakeEngine"; }
    std::unique_ptr<EngineControl> make(uint32_t protocol, uint32_t layout) override {
        FakeEngine::Config cfg;
        cfg.tracks = 4;
        cfg.protocol_version = protocol;
        cfg.state_layout_version = layout;
        cfg.clock = [this]() { return now_; };
        return std::make_unique<FakeEngine>(cfg);
    }
    uint64_t now_ns() override { return now_; }
    void wait_ns(uint64_t ns) override { now_ += ns; }
    void kill(EngineControl& e) override { static_cast<FakeEngine&>(e).die(); }
    bool has_signal() const override { return true; }
    bool set_takes_fail(EngineControl& e, bool fail) override {
        static_cast<FakeEngine&>(e).script_take_failure(fail);
        return true;
    }
    bool set_inputs_present(EngineControl& e, bool present) override {
        static_cast<FakeEngine&>(e).script_inputs(present, "the USB interface was unplugged");
        return true;
    }
    bool edit_in_plugin_editor(EngineControl& e, uint64_t insert, const std::string& key,
                               float value, bool close) override {
        auto& fake = static_cast<FakeEngine&>(e);
        fake.script_editor_change(insert, key, value);
        if (close) {
            fake.script_editor_closed(insert);
        } else {
            fake.script_editor_gesture_end(insert);
        }
        return true;
    }
    bool set_plugin_state(EngineControl& e, uint64_t insert, const std::string& sha256,
                          uint64_t bytes) override {
        static_cast<FakeEngine&>(e).script_plugin_state(insert, sha256, bytes);
        return true;
    }

private:
    uint64_t now_ = 5'000'000'000;
};

void deterministic() {
    uint64_t now = 1'000'000'000;
    FakeEngine::Config cfg;
    cfg.clock = [&now]() { return now; };
    FakeEngine e(cfg);
    std::string error;
    CHECK(e.connect(error));
    CHECK(e.hello().track_count == 18);
    // Channel 10 (index 9) goes over full scale in the first 50 ms of every
    // 7 s. A reader that sampled only its tick's end would miss it; the
    // peak is the maximum over the interval.
    Inbox in;
    now += 30'000'000;
    e.publish();
    now += 40'000'000;
    e.poll(in);
    CHECK(FakeEngine::level_at(9, 0.07) < 1.0f);
    CHECK(e.state().take_peak(9) >= 1.0f);
    now += 16'000'000;
    e.poll(in);
    CHECK(e.state().take_peak(9) < 1.0f);
    CHECK(e.state().track(9).clip_count >= 1);

    // A scripted peak and event.
    e.script_peak(2, 0.75f);
    EngineEvent ev;
    ev.kind = EventKind::Notice;
    ev.text = "recording from Built-in Audio (onboard)";
    e.post_event(ev);
    e.poll(in);
    CHECK(e.state().take_peak(2) == 0.75f);
    CHECK(in.events.size() == 1 && in.events[0].text == ev.text);

    // Death at a time: no publish, no reply after it.
    e.die_at(now + 1);
    now += 2;
    const uint64_t hb = e.state().transport().heartbeat;
    CommandRequest r;
    r.command = "track.arm_all";
    e.send(r);
    in.clear();
    e.poll(in);
    CHECK(in.replies.empty());
    CHECK(e.state().transport().heartbeat == hb);
}

bool mentions(const std::string& text, const char* needle) {
    if (text.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle, text.c_str());
    return false;
}

// The fake's plugins, as a UI under development sees them: what it refuses
// and in which words, the latency and fault states it shows, a rescan, and
// a session opened with a plugin that is not installed.
void scripted_plugins() {
    FakeEngine::Config cfg;
    cfg.tracks = 2;
    FakeEngine e(cfg);
    std::string error;
    CHECK(e.connect(error));
    Inbox in;
    sonora::model::Replica replica;
    std::size_t fed = 0;
    const auto poll = [&]() {
        e.poll(in);
        for (; fed < in.model.size(); ++fed) {
            sonora::model::ReplicationUpdate u;
            const ModelUpdate& m = in.model[fed];
            u.kind = m.kind == ModelUpdateKind::Snapshot
                         ? sonora::model::ReplicationUpdate::Kind::Snapshot
                         : sonora::model::ReplicationUpdate::Kind::Op;
            u.revision = m.revision;
            u.undo_floor = m.undo_floor;
            u.next_id = m.next_id;
            u.bytes = m.bytes;
            u.label = m.label;
            std::string why;
            CHECK(replica.receive(u, why));
        }
    };
    const auto reply_to = [&](uint64_t seq) -> CommandReply {
        poll();
        for (const CommandReply& r : in.replies) {
            if (r.seq == seq) return r;
        }
        return CommandReply{seq, DispatchResult::UnknownCommand, "no reply"};
    };
    const auto apply = [&](const sonora::model::Operation& op) {
        ModelRequest m;
        m.action = ModelAction::Apply;
        CHECK(sonora::model::encode_operation(op, m.op, error));
        return reply_to(e.submit(m));
    };

    poll();
    CHECK(replica.synced());
    CHECK(in.catalogs.size() == 1 && in.catalogs[0].generation == 1);
    const std::vector<PluginInfo> catalog = in.catalogs[0].plugins;
    CHECK(catalog.size() == 4);
    const auto ref = [&](std::size_t i) {
        return sonora::model::PluginRef{static_cast<sonora::model::PluginFormat>(catalog[i].format),
                                        catalog[i].id, catalog[i].name, catalog[i].version};
    };

    sonora::model::AddInsert add;
    add.track = replica.current().edit->tracks[0]->id;
    add.insert = replica.current().next_id;
    add.slot = 0;
    add.plugin = ref(1);   // stereo, onto a mono track
    CommandReply r = apply(add);
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "the track has 1 channel"));
    add.plugin = ref(3);
    r = apply(add);
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "cannot be used"));
    add.plugin.id = "urn:sonora:fake:nowhere";
    r = apply(add);
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "is installed"));
    add.plugin = ref(0);
    r = apply(add);
    CHECK(r.result == DispatchResult::Ok);
    const uint64_t gain = add.insert;

    // Running: one IPC block plus what the plugin reports, on the track and
    // as the session's compensation.
    e.script_plugin_latency(catalog[0].id, 32);
    poll();
    TrackSnapshot t = e.state().track(0);
    CHECK((t.inserts[0].flags & (kInsertPresent | kInsertActive | kInsertRunning)) ==
          (kInsertPresent | kInsertActive | kInsertRunning));
    CHECK(t.inserts[0].latency_frames == 32 && t.latency_frames == 32 + cfg.buffer_frames);
    CHECK(e.state().transport().compensation_frames == 32 + cfg.buffer_frames);
    CHECK(!e.state().track(1).inserts[0].present());

    // By key, never by index (catalog order may change with a rescan).
    PluginParameterChange c;
    c.insert = gain;
    c.key = "gain";
    c.value = 6.0f;
    CHECK(reply_to(e.send(c)).result == DispatchResult::Ok);
    CHECK(e.plugin_value(gain, "gain") == 6.0f);
    c.value = 30.0f;
    r = reply_to(e.send(c));
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "Gain must be from -24 to 24"));
    CHECK(e.plugin_value(gain, "gain") == 6.0f);
    c.key = "0";
    c.value = 0.0f;
    r = reply_to(e.send(c));
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "no parameter \"0\""));

    // A value moved in the plugin's own editor reaches the document, and so
    // every replica, when the gesture ends; nothing before (H1, #46).
    const std::size_t updates = in.model.size();
    e.script_editor_change(gain, "gain", -3.0f);
    poll();
    CHECK(in.model.size() == updates);
    e.script_editor_gesture_end(gain);
    poll();
    CHECK(in.model.size() == updates + 1 && in.model.back().kind == ModelUpdateKind::Op);
    const auto value_of = [&](const char* key) {
        const sonora::model::InsertMix* m = replica.current().mix.find_insert(gain);
        if (m == nullptr) return std::numeric_limits<float>::quiet_NaN();
        for (const auto& p : m->params) {
            if (p.key == key) return p.value;
        }
        return std::numeric_limits<float>::quiet_NaN();
    };
    CHECK(value_of("gain") == -3.0f && e.plugin_value(gain, "gain") == -3.0f);
    // What moved without a gesture end is recorded when the editor closes,
    // beside what was recorded before.
    e.script_editor_change(gain, "invert", 1.0f);
    e.script_editor_closed(gain);
    poll();
    CHECK(in.model.size() == updates + 2);
    CHECK(value_of("invert") == 1.0f && value_of("gain") == -3.0f);
    e.script_editor_closed(gain);   // nothing moved since: nothing sent
    poll();
    CHECK(in.model.size() == updates + 2);

    // A flush records the plugin's state before its reply (M1, #46).
    const std::string digest(64, 'c');
    e.script_plugin_state(gain, digest, 4096);
    ModelRequest flush;
    flush.action = ModelAction::FlushPluginState;
    CHECK(reply_to(e.submit(flush)).result == DispatchResult::Ok);
    CHECK(in.model.size() == updates + 3);
    const sonora::model::InsertMix* saved = replica.current().mix.find_insert(gain);
    CHECK(saved != nullptr && saved->state.has_value() && saved->state->sha256 == digest &&
          saved->state->bytes == 4096);
    CHECK(reply_to(e.submit(flush)).result == DispatchResult::Ok);   // nothing new
    CHECK(in.model.size() == updates + 3);
    flush.op = "999999";
    r = reply_to(e.submit(flush));
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "insert 999999"));
    flush.op = "1x";
    r = reply_to(e.submit(flush));
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "not an insert id"));

    // A fault takes the chain down and says so, by insert id, naming plugin,
    // track and slot; recovery clears it.
    e.script_insert_fault(gain, "crashed (SIGSEGV)");
    poll();
    CHECK(!in.events.empty() && in.events.back().kind == EventKind::PluginFault);
    CHECK(in.events.back().insert == gain && in.events.back().track == -1);
    CHECK(mentions(in.events.back().text, "Fake Gain (Audio 1, slot A): crashed"));
    t = e.state().track(0);
    CHECK(t.inserts[0].faulted() && (t.inserts[0].flags & kInsertRunning) == 0);
    CHECK(t.inserts[0].misses == 1 && t.latency_frames == 0);
    // Its state cannot be had while it is down.
    e.script_plugin_state(gain, std::string(64, 'd'), 10);
    flush.op = std::to_string(gain);
    r = reply_to(e.submit(flush));
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "its host is down"));
    e.script_insert_fault(gain, "");
    poll();
    CHECK((e.state().track(0).inserts[0].flags & kInsertRunning) != 0);
    CHECK(in.events.back().kind == EventKind::PluginFault && in.events.back().insert == gain &&
          in.events.back().text.empty());

    // A rescan without the plugin: a new catalog, and the insert missing.
    std::vector<PluginInfo> fewer(catalog.begin() + 1, catalog.end());
    e.script_catalog(fewer);
    poll();
    CHECK(in.catalogs.size() == 2 && in.catalogs[1].generation == 2 &&
          in.catalogs[1].plugins.size() == 3);
    CHECK((e.state().track(0).inserts[0].flags & kInsertMissing) != 0);
    CHECK(in.events.back().insert == gain && mentions(in.events.back().text, "is not installed"));

    // A session opened with it: the old fault cleared, the insert kept, and
    // named again as the new session's.
    sonora::model::Snapshot doc = replica.current();
    std::string bytes;
    CHECK(sonora::model::serialize(doc, sonora::model::WriteInfo{"test", 1}, bytes, error));
    const std::size_t events = in.events.size();
    ModelRequest load;
    load.action = ModelAction::Load;
    load.op = bytes;
    CHECK(reply_to(e.submit(load)).result == DispatchResult::Ok);
    CHECK(in.events.size() == events + 2);
    CHECK(in.events[events].insert == gain && in.events[events].text.empty());
    CHECK(in.events.back().kind == EventKind::PluginFault && in.events.back().insert == gain);
    CHECK(mentions(in.events.back().text, "is not installed here"));
    CHECK(replica.current().edit->tracks[0]->inserts.size() == 1);
}

}  // namespace

int main() {
    FakeRig rig;
    const int failed = conformance::run(rig);
    if (failed != 0) {
        std::fprintf(stderr, "%d conformance check(s) failed on FakeEngine\n", failed);
        ++failures;
    }
    deterministic();
    scripted_plugins();
    if (failures) return EXIT_FAILURE;
    std::printf("api fake engine: ok\n");
    return EXIT_SUCCESS;
}
