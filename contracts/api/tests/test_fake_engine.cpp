// SPDX-License-Identifier: GPL-3.0-or-later
//
// FakeEngine against the control API's conformance suite, the same suite
// engine/control runs on InProcessEngine, plus what only a fake can be
// asked to do: be deterministic, script an event, die on cue.
#include "sonora/api/fake_engine.h"

#include <cstdio>
#include <cstdlib>
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

    PluginParameterChange c;
    c.insert = gain;
    c.param = 0;
    c.value = 6.0f;
    CHECK(reply_to(e.send(c)).result == DispatchResult::Ok);
    CHECK(e.plugin_value(gain, 0) == 6.0f);
    c.value = 30.0f;
    r = reply_to(e.send(c));
    CHECK(r.result == DispatchResult::Failed && mentions(r.message, "Gain must be from -24 to 24"));
    CHECK(e.plugin_value(gain, 0) == 6.0f);

    // A fault takes the chain down and says so, naming plugin and slot.
    e.script_insert_fault(gain, "crashed (SIGSEGV)");
    poll();
    CHECK(!in.events.empty() && in.events.back().kind == EventKind::PluginFault);
    CHECK(in.events.back().track == 0 && in.events.back().frame == 0);
    CHECK(mentions(in.events.back().text, "Fake Gain (slot A): crashed"));
    t = e.state().track(0);
    CHECK(t.inserts[0].faulted() && (t.inserts[0].flags & kInsertRunning) == 0);
    CHECK(t.inserts[0].misses == 1 && t.latency_frames == 0);
    e.script_insert_fault(gain, "");
    poll();
    CHECK((e.state().track(0).inserts[0].flags & kInsertRunning) != 0);

    // A rescan without the plugin: a new catalog, and the insert missing.
    std::vector<PluginInfo> fewer(catalog.begin() + 1, catalog.end());
    e.script_catalog(fewer);
    poll();
    CHECK(in.catalogs.size() == 2 && in.catalogs[1].generation == 2 &&
          in.catalogs[1].plugins.size() == 3);
    CHECK((e.state().track(0).inserts[0].flags & kInsertMissing) != 0);

    // A session opened with it: kept, and named.
    sonora::model::Snapshot doc = replica.current();
    std::string bytes;
    CHECK(sonora::model::serialize(doc, sonora::model::WriteInfo{"test", 1}, bytes, error));
    const std::size_t events = in.events.size();
    ModelRequest load;
    load.action = ModelAction::Load;
    load.op = bytes;
    CHECK(reply_to(e.submit(load)).result == DispatchResult::Ok);
    CHECK(in.events.size() == events + 1 && in.events.back().kind == EventKind::PluginFault);
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
