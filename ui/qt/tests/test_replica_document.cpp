// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplicaDocument on FakeEngine through ControlAdapter (ADR-0007 M7): the
// Edit window's document is the engine's. An edit shows at once and is
// still there once the engine has it; another interface's edit arrives;
// an edit the engine refuses is taken back with its reason; undo and redo
// go to the engine; File > Open sends the session to the engine. And the
// model path's replies never reach the command path's status line.
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "replica_document.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/op_codec.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

struct Rig {
    api::testing::FakeEngine fake;
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    std::vector<CommandOutcome> outcomes;
    std::vector<std::string> refused;
    int updates = 0;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        return c;
    }
    Rig() : fake(config()) {
        doc.on_refused = [this](const std::string& why) { refused.push_back(why); };
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) ++updates;
        });
    }
    // One interface tick.
    void tick() { engine.poll(outcomes); }
};

model::AddMarker marker(model::Id id, const char* name) {
    model::AddMarker m;
    m.marker.id = id;
    m.marker.position = 4800;
    m.marker.name = name;
    return m;
}

std::size_t markers(const ReplicaDocument& d) { return d.snapshot().edit->markers->markers.size(); }

int edits_show_at_once_and_stay() {
    Rig r;
    std::string error;
    CHECK(r.engine.connect(error));
    r.tick();
    CHECK(r.doc.synced());
    std::string why;
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Verse"), why));
    CHECK(markers(r.doc) == 1);   // before the engine has seen it
    CHECK(r.doc.in_flight() == 1);
    r.tick();
    CHECK(r.doc.in_flight() == 0 && markers(r.doc) == 1);
    // The command path's status line never sees a model reply.
    CHECK(r.outcomes.empty());

    CHECK(r.doc.undo(why));
    CHECK(markers(r.doc) == 0);
    r.tick();
    CHECK(markers(r.doc) == 0 && r.doc.can_redo());
    CHECK(r.doc.redo(why));
    r.tick();
    CHECK(markers(r.doc) == 1 && r.doc.in_flight() == 0);
    CHECK(!r.doc.redo(why) && !why.empty());
    CHECK(r.refused.empty());
    return 0;
}

int another_interfaces_edit_arrives_and_a_clash_is_refused() {
    Rig r;
    std::string error;
    CHECK(r.engine.connect(error));
    r.tick();
    const model::Id id = r.doc.snapshot().next_id;
    // Another interface takes the id first.
    api::ModelRequest other;
    other.action = api::ModelAction::Apply;
    CHECK(model::encode_operation(marker(id, "Theirs"), other.op, error));
    r.fake.submit(other);
    std::string why;
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Ours"), why));
    CHECK(markers(r.doc) == 1 && r.doc.snapshot().edit->markers->markers[0].name == "Ours");
    r.tick();
    // Theirs is in the engine; ours was refused, taken back, and said.
    CHECK(r.doc.in_flight() == 0);
    CHECK(markers(r.doc) == 1 && r.doc.snapshot().edit->markers->markers[0].name == "Theirs");
    CHECK(r.refused.size() == 1);
    CHECK(r.updates >= 1);
    return 0;
}

int open_sends_the_session_to_the_engine() {
    Rig r;
    std::string error;
    CHECK(r.engine.connect(error));
    r.tick();
    std::string why;
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Old"), why));
    r.tick();
    CHECK(r.doc.can_undo());
    model::Snapshot demo = make_demo_session(4, 48000);
    const std::size_t regions = demo.edit->tracks[0]->playlists[0]->regions.size();
    r.doc.reset(demo);
    CHECK(r.doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == regions);
    CHECK(!r.doc.can_undo());
    r.tick();
    CHECK(r.doc.in_flight() == 0 && r.refused.empty());
    CHECK(r.doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == regions);
    CHECK(markers(r.doc) == 0);
    CHECK(!r.doc.can_undo());

    // A session at another rate is refused by the engine and taken back.
    r.doc.reset(make_empty_session(2, 44100));
    r.tick();
    CHECK(r.refused.size() == 1);
    CHECK(r.doc.snapshot().settings.sample_rate == 48000);
    CHECK(r.doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == regions);
    return 0;
}

}  // namespace

int main() {
    if (edits_show_at_once_and_stay() != 0) return 1;
    if (another_interfaces_edit_arrives_and_a_clash_is_refused() != 0) return 1;
    if (open_sends_the_session_to_the_engine() != 0) return 1;
    std::printf("replica document: ok\n");
    return 0;
}
