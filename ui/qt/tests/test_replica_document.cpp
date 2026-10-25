// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplicaDocument on FakeEngine through ControlAdapter (ADR-0007 M7): the
// Edit window's document is the engine's. An edit shows at once and is
// still there once the engine has it; another interface's edit arrives;
// an edit the engine refuses is taken back with its reason; undo and redo
// go to the engine; File > Open sends the session to the engine. And the
// model path's replies never reach the command path's status line. And when
// the replica loses track: one Resync per loss, the last good document kept
// on screen meanwhile, and edits refused until the Snapshot.
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "edit_controller.h"
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
    explicit Rig(api::testing::FakeEngine::Config c = config()) : fake(std::move(c)) {
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
    CHECK(r.doc.reset(demo, why));
    CHECK(r.doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == regions);
    CHECK(!r.doc.can_undo());
    r.tick();
    CHECK(r.doc.in_flight() == 0 && r.refused.empty());
    CHECK(r.doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == regions);
    CHECK(markers(r.doc) == 0);
    CHECK(!r.doc.can_undo());

    // A session at another rate is refused by the engine and taken back.
    CHECK(r.doc.reset(make_empty_session(2, 44100), why));
    r.tick();
    CHECK(r.refused.size() == 1);
    CHECK(r.doc.snapshot().settings.sample_rate == 48000);
    CHECK(r.doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == regions);
    return 0;
}

// The link going down drops what the engine never answered, even before its
// first Snapshot, and refuses edits until it is back.
int a_lost_link_drops_what_was_not_answered() {
    Rig r;
    std::string why;
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Unsent"), why));
    CHECK(markers(r.doc) == 1 && r.doc.in_flight() == 1);
    CHECK(r.doc.set_link(false));
    CHECK(markers(r.doc) == 0 && r.doc.in_flight() == 0);
    CHECK(r.refused.size() == 1);
    CHECK(!r.doc.submit(marker(r.doc.reserve_id(), "Offline"), why) && !why.empty());
    CHECK(!r.doc.reset(make_demo_session(4, 48000), why));
    CHECK(!r.doc.set_link(true));
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Back"), why));
    CHECK(markers(r.doc) == 1);
    return 0;
}


// --- losing track (PR #75 ui-engineer review) -------------------------------
//
// Driven by hand, with common/model's Document as the engine, so a test can
// lose an update on purpose and choose what arrives in which tick.

api::ModelUpdate to_wire(const model::ReplicationUpdate& u) {
    api::ModelUpdate m;
    switch (u.kind) {
        case model::ReplicationUpdate::Kind::Op: m.kind = api::ModelUpdateKind::Op; break;
        case model::ReplicationUpdate::Kind::Undo: m.kind = api::ModelUpdateKind::Undo; break;
        case model::ReplicationUpdate::Kind::Redo: m.kind = api::ModelUpdateKind::Redo; break;
        case model::ReplicationUpdate::Kind::Snapshot: m.kind = api::ModelUpdateKind::Snapshot; break;
    }
    m.revision = u.revision;
    m.undo_floor = u.undo_floor;
    m.next_id = u.next_id;
    m.bytes = u.bytes;
    m.label = u.label;
    return m;
}

struct HandRig {
    model::Document engine{make_demo_session(4, 48000)};
    std::vector<api::ModelRequest> sent;
    uint64_t seq = 0;
    ReplicaDocument doc{[this](const api::ModelRequest& r) {
                            sent.push_back(r);
                            sent.back().seq = ++seq;
                            return seq;
                        },
                        make_empty_session(4, 48000)};
    std::vector<std::string> refused;

    HandRig() { doc.on_refused = [this](const std::string& why) { refused.push_back(why); }; }

    api::ModelUpdate snapshot() {
        model::ReplicationUpdate u;
        std::string error;
        engine.snapshot(u, error);
        return to_wire(u);
    }
    // Another interface's edit, applied by the engine.
    api::ModelUpdate theirs(const char* name) {
        model::ReplicationUpdate u;
        std::string error;
        engine.apply(marker(engine.current().next_id, name), u, error);
        return to_wire(u);
    }
    std::size_t resyncs() const {
        std::size_t n = 0;
        for (const auto& r : sent) n += r.action == api::ModelAction::Resync ? 1 : 0;
        return n;
    }
};

std::size_t regions(const ReplicaDocument& d) {
    return d.snapshot().edit->tracks[0]->playlists[0]->regions.size();
}

int a_loss_sends_one_resync_and_keeps_the_last_document() {
    HandRig r;
    CHECK(r.doc.receive({r.snapshot()}, {}));
    CHECK(r.doc.synced());
    const std::size_t demo_regions = regions(r.doc);
    CHECK(demo_regions > 0);   // the placeholder has none, so it would show

    // Three updates; the first is lost. The two after it each fail, and the
    // whole document is asked for once.
    r.theirs("One");
    const api::ModelUpdate two = r.theirs("Two");
    const api::ModelUpdate three = r.theirs("Three");
    r.doc.receive({two, three}, {});
    CHECK(r.resyncs() == 1 && r.doc.resyncs_sent() == 1);
    // A later tick's update, still before the Snapshot, asks nothing more.
    r.doc.receive({r.theirs("Four")}, {});
    CHECK(r.resyncs() == 1);

    // Meanwhile the session is still on screen, not an empty placeholder,
    // and an edit made against it is refused, by name.
    CHECK(r.doc.resyncing() && !r.doc.synced());
    CHECK(regions(r.doc) == demo_regions);
    std::string why;
    CHECK(!r.doc.submit(marker(r.doc.reserve_id(), "Mine"), why));
    CHECK(why.find("catching up") != std::string::npos);
    CHECK(!r.doc.undo(why) && why.find("catching up") != std::string::npos);

    // The Snapshot ends the loss: all four markers, edits allowed again.
    CHECK(r.doc.receive({r.snapshot()}, {}));
    CHECK(r.doc.synced() && !r.doc.resyncing());
    CHECK(markers(r.doc) == 4);
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Mine"), why));

    // A second loss is a second Resync: one per loss, not one ever.
    r.theirs("Five");
    r.doc.receive({r.theirs("Six")}, {});
    CHECK(r.resyncs() == 2);
    // A new connection forgets the Resync the old one was asked for: its
    // own Snapshot comes anyway, and a loss after that asks again.
    r.doc.set_link(false);
    r.doc.set_link(true);
    r.doc.receive({r.snapshot()}, {});
    r.theirs("Seven");
    r.doc.receive({r.theirs("Eight")}, {});
    CHECK(r.resyncs() == 3);
    return 0;
}

// An edit the engine accepted, whose update was lost with the others, stays
// on screen over the last good document until the Snapshot that holds it.
int an_accepted_edit_stays_through_a_loss() {
    HandRig r;
    r.doc.receive({r.snapshot()}, {});
    std::string why;
    const model::Operation ours = marker(r.doc.reserve_id(), "Ours");
    CHECK(r.doc.submit(ours, why));
    const uint64_t seq = r.sent.back().seq;
    model::ReplicationUpdate applied;
    std::string error;
    CHECK(r.engine.apply(ours, applied, error));   // its update is lost
    api::CommandReply ok;
    ok.seq = seq;
    r.doc.receive({r.theirs("Theirs")}, {ok});
    CHECK(r.doc.resyncing());
    CHECK(markers(r.doc) == 1 && r.doc.snapshot().edit->markers->markers[0].name == "Ours");
    CHECK(r.doc.receive({r.snapshot()}, {}));
    CHECK(r.doc.in_flight() == 0 && markers(r.doc) == 2);
    CHECK(r.refused.empty());
    return 0;
}

// PR #86 review: one tick brings an accepted Undo's update, which applies,
// then an update that fails. The Undo is in the replica's last good document
// already; replayed on top of it, two steps would show as undone.
int an_applied_undo_is_not_shown_twice_while_lost() {
    HandRig r;
    r.doc.receive({r.snapshot()}, {});
    std::string why, error;
    for (const char* name : {"A", "B"}) {
        const model::Operation op = marker(r.doc.reserve_id(), name);
        CHECK(r.doc.submit(op, why));
        api::CommandReply ok;
        ok.seq = r.sent.back().seq;
        model::ReplicationUpdate u;
        CHECK(r.engine.apply(op, u, error));
        r.doc.receive({to_wire(u)}, {ok});
    }
    CHECK(markers(r.doc) == 2 && r.doc.in_flight() == 0);

    CHECK(r.doc.undo(why));
    CHECK(markers(r.doc) == 1);
    api::CommandReply ok;
    ok.seq = r.sent.back().seq;
    model::ReplicationUpdate undone;
    CHECK(r.engine.undo(undone, error));
    r.theirs("Lost");
    r.doc.receive({to_wire(undone), r.theirs("Fails")}, {ok});
    CHECK(r.doc.resyncing());
    CHECK(markers(r.doc) == 1);   // B undone once, not A as well
    CHECK(r.doc.receive({r.snapshot()}, {}));
    CHECK(r.doc.in_flight() == 0 && markers(r.doc) == 3);
    return 0;
}

// PR #86 review: the engine refuses the Resync (a document it cannot
// snapshot). The interface must not stay "catching up" for good: the reason
// is said, and the next edit asks again rather than waiting on nothing.
int a_refused_resync_is_said_and_asked_again() {
    HandRig r;
    r.doc.receive({r.snapshot()}, {});
    r.theirs("Lost");
    r.doc.receive({r.theirs("Fails")}, {});
    CHECK(r.resyncs() == 1 && r.doc.resyncing());
    api::CommandReply no;
    no.seq = r.sent.back().seq;
    no.result = DispatchResult::Failed;
    no.message = "the document is too large to send";
    r.doc.receive({}, {no});
    CHECK(r.refused.size() == 1 && r.refused[0].find("too large") != std::string::npos);
    // Still behind the engine, so the edit is refused; but it asks again.
    std::string why;
    CHECK(!r.doc.submit(marker(r.doc.reserve_id(), "Mine"), why));
    CHECK(r.resyncs() == 2);
    CHECK(why.find("too large") != std::string::npos);
    // Only one Resync is out at a time.
    CHECK(!r.doc.undo(why));
    CHECK(r.resyncs() == 2);
    // The second one is answered: edits allowed again.
    CHECK(r.doc.receive({r.snapshot()}, {}));
    CHECK(r.doc.synced());
    CHECK(r.doc.submit(marker(r.doc.reserve_id(), "Mine"), why));
    return 0;
}

// What the Edit menu says: the engine's step labels, a compound's included.
int undo_and_redo_labels_follow_the_engine() {
    HandRig r;
    r.doc.receive({r.snapshot()}, {});
    CHECK(r.doc.undo_label().empty() && r.doc.redo_label().empty());
    std::string why;
    model::Compound c;
    c.label = "Paste";
    c.ops.push_back(model::AddMarker{model::Marker{r.doc.reserve_id(), 4800, "A", 0}});
    c.ops.push_back(model::AddMarker{model::Marker{r.doc.reserve_id(), 9600, "B", 0}});
    CHECK(r.doc.submit(c, why));
    CHECK(r.doc.undo_label() == "Paste");
    CHECK(r.doc.undo(why));
    CHECK(r.doc.redo_label() == "Paste" && r.doc.undo_label().empty());
    return 0;
}

// The Undo History window's jump on the engine's document: each step its own
// Undo or Redo, all shown at once and in flight together, and the engine
// ends where the window does. Refused outright with the link down.
int a_history_jump_is_one_request_a_step() {
    Rig r;
    std::string error;
    CHECK(r.engine.connect(error));
    r.tick();
    std::string why;
    for (const char* name : {"One", "Two", "Three", "Four"}) {
        CHECK(r.doc.submit(marker(r.doc.reserve_id(), name), why));
    }
    r.tick();
    CHECK(r.doc.in_flight() == 0 && r.doc.history().undo_depth() == 4);
    EditController c(r.doc);
    CHECK(c.undo_to(1, why));
    CHECK(markers(r.doc) == 1 && r.doc.in_flight() == 3);
    r.tick();
    CHECK(r.doc.in_flight() == 0 && markers(r.doc) == 1 && r.refused.empty());
    CHECK(r.doc.history().undo_depth() == 1 && r.doc.redo_label() == "AddMarker");
    CHECK(c.undo_to(3, why));
    r.tick();
    CHECK(markers(r.doc) == 3 && r.doc.history().undo_depth() == 3 && r.refused.empty());
    CHECK(!c.undo_to(r.doc.history().step_count(), why) && !why.empty());
    CHECK(!r.doc.set_link(false));
    why.clear();
    CHECK(!c.undo_to(0, why) && !why.empty());
    CHECK(markers(r.doc) == 3);
    return 0;
}

}  // namespace

// One MIDI track on `base` whose region holds `sysex` messages of 4 KiB:
// the cheapest way to a large bundle.
model::Snapshot with_midi(model::Snapshot base, std::size_t sysex, std::size_t events = 0) {
    model::History h(std::move(base));
    model::AddTrack t;
    t.track = h.current().next_id;
    t.playlist = t.track + 1;
    t.name = "Synth";
    t.playlist_name = "Synth.01";
    t.kind = model::TrackKind::Midi;
    t.channels = 0;
    auto seq = std::make_shared<model::MidiSequence>();
    std::vector<std::uint8_t> message(model::kMaxSysexBytes, 0x11);
    message.front() = 0xF0;
    message.back() = 0xF7;
    for (std::size_t i = 0; i < sysex; ++i) {
        seq->sysex.push_back(model::MidiSysEx{static_cast<model::Ticks>(i), message});
    }
    for (std::size_t i = 0; i < events; ++i) {
        seq->events.push_back(model::MidiEvent{static_cast<model::Ticks>(i), 0xB0, 1, 2});
    }
    model::MidiRegion r;
    r.id = t.track + 2;
    r.name = "Synth_01";
    r.length = model::kTicksPerQuarter;
    r.data = seq;
    std::string why;
    if (!h.apply(t, why) || !h.apply(model::PlaceMidiRegion{t.playlist, r, 0, {}}, why)) {
        std::fprintf(stderr, "  %s\n", why.c_str());
    }
    return h.current();
}

// Architect, #109: a session whose bundle is over what a Load may carry is
// refused here, by name, before anything is shown or sent, instead of being
// shown while the engine never gets it.
int a_load_over_the_cap_is_refused_before_it_is_shown() {
    Rig r;
    std::string error;
    CHECK(r.engine.connect(error));
    r.tick();
    const std::size_t tracks = r.doc.snapshot().edit->tracks.size();
    // 16,400 messages of 4,096 bytes: just over 64 MiB once encoded.
    const model::Snapshot big = with_midi(make_empty_session(4, 48000), 16400);
    std::string why;
    CHECK(!r.doc.reset(big, why));
    CHECK(why.find("over the 67108864 bytes a Load can carry") != std::string::npos);
    CHECK(why.find("once a Load travels in parts") != std::string::npos);
    CHECK(r.doc.snapshot().edit->tracks.size() == tracks && r.doc.in_flight() == 0);
    r.tick();
    CHECK(r.doc.snapshot().edit->tracks.size() == tracks && r.refused.empty());
    // Under the cap it is sent, and shown.
    CHECK(r.doc.reset(with_midi(make_empty_session(4, 48000), 10), why));
    r.tick();
    CHECK(r.doc.in_flight() == 0 && r.refused.empty());
    CHECK(r.doc.snapshot().edit->tracks.size() == 5);
    return 0;
}

// A bundle the engine refuses (here, over its midi_max_entries) is taken
// back with the engine's reason: the screen never keeps a session the
// engine did not load.
int a_load_the_engine_refuses_is_taken_back() {
    api::testing::FakeEngine::Config c = Rig::config();
    c.midi_max_entries = 100;
    Rig r(c);
    std::string error;
    CHECK(r.engine.connect(error));
    r.tick();
    const std::size_t tracks = r.doc.snapshot().edit->tracks.size();
    std::string why;
    CHECK(r.doc.reset(with_midi(make_empty_session(4, 48000), 0, 101), why));
    CHECK(r.doc.snapshot().edit->tracks.size() == 5);   // shown at once
    r.tick();
    CHECK(r.refused.size() == 1);
    CHECK(r.refused[0].find("midi_max_entries of 100") != std::string::npos);
    CHECK(r.doc.in_flight() == 0 && r.doc.snapshot().edit->tracks.size() == tracks);
    return 0;
}

int main() {
    if (a_load_over_the_cap_is_refused_before_it_is_shown() != 0) return 1;
    if (a_load_the_engine_refuses_is_taken_back() != 0) return 1;
    if (a_lost_link_drops_what_was_not_answered() != 0) return 1;
    if (edits_show_at_once_and_stay() != 0) return 1;
    if (another_interfaces_edit_arrives_and_a_clash_is_refused() != 0) return 1;
    if (open_sends_the_session_to_the_engine() != 0) return 1;
    if (a_loss_sends_one_resync_and_keeps_the_last_document() != 0) return 1;
    if (an_accepted_edit_stays_through_a_loss() != 0) return 1;
    if (an_applied_undo_is_not_shown_twice_while_lost() != 0) return 1;
    if (a_refused_resync_is_said_and_asked_again() != 0) return 1;
    if (undo_and_redo_labels_follow_the_engine() != 0) return 1;
    if (a_history_jump_is_one_request_a_step() != 0) return 1;
    std::printf("replica document: ok\n");
    return 0;
}
