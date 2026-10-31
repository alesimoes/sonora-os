// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output routing in the document (ADR-0024): SetOutput as an undo step,
// feedback through sends and outputs refused naming the path, a bus that is
// an output kept, aux solo as mix state, and outputs surviving the codec,
// the undo window and the session file (its migration: test_session_file).
#include <memory>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

AddBus add_bus(Ids& ids, const std::string& name) {
    AddBus op;
    op.bus = ids();
    op.name = name;
    return op;
}

AddSend add_send(Ids& ids, Id strip, std::uint8_t slot, Id bus) {
    AddSend op;
    op.strip = strip;
    op.send = ids();
    op.slot = slot;
    op.bus = bus;
    return op;
}

bool refused(History& h, const Operation& op, const char* says) {
    const Snapshot before = h.current();
    std::string error;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", says);
        return false;
    }
    if (!mentions(error, says)) std::fprintf(stderr, "  refused with \"%s\"\n", error.c_str());
    return mentions(error, says) && h.current() == before;
}

Id output_of_track(const Snapshot& s, std::size_t t) { return s.edit->tracks[t]->output; }
Id output_of_bus(const Snapshot& s, std::size_t b) { return s.edit->buses[b].output; }

int test_set_output_and_undo() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack kick = add_track(ids, "Kick");
    const AddTrack snare = add_track(ids, "Snare");
    CHECK_APPLY(h, kick);
    CHECK_APPLY(h, snare);
    const AddBus drums = add_bus(ids, "Drums");
    CHECK_APPLY(h, drums);
    CHECK(output_of_track(h.current(), 0) == kMasterOutput);
    CHECK(output_of_bus(h.current(), 0) == kMasterOutput);

    const SetOutput route{kick.track, drums.bus};
    CHECK(is_undoable(route));
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, route);
    CHECK_APPLY(h, (SetOutput{snare.track, drums.bus}));
    CHECK(h.undo_depth() == depth + 2);
    CHECK(output_of_track(h.current(), 0) == drums.bus);
    CHECK(output_of_track(h.current(), 1) == drums.bus);
    CHECK(invariants_hold(h.current()));

    // Undo puts the routing back one step at a time; redo again.
    CHECK(h.undo());
    CHECK(output_of_track(h.current(), 1) == kMasterOutput);
    CHECK(output_of_track(h.current(), 0) == drums.bus);
    CHECK(h.redo());
    CHECK(output_of_track(h.current(), 1) == drums.bus);

    // Back to the Master.
    CHECK_APPLY(h, (SetOutput{kick.track, kMasterOutput}));
    CHECK(output_of_track(h.current(), 0) == kMasterOutput);

    // A duplicate plays where its source does.
    DuplicateTrack dup;
    dup.source = snare.track;
    dup.track = ids();
    dup.name = "Snare.dup1";
    CHECK_APPLY(h, dup);
    ids.next = h.current().next_id;
    CHECK(output_of_track(h.current(), 2) == drums.bus);
    CHECK(invariants_hold(h.current()));
    return 0;
}

// ADR-0024 §3: sends and outputs make one graph, which stays acyclic.
int test_feedback_through_outputs_is_refused() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddBus a = add_bus(ids, "A");
    const AddBus b = add_bus(ids, "B");
    const AddBus c = add_bus(ids, "C");
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);
    CHECK_APPLY(h, c);
    CHECK(refused(h, SetOutput{a.bus, a.bus}, "a bus cannot be its own output"));
    CHECK_APPLY(h, (SetOutput{a.bus, b.bus}));
    CHECK_APPLY(h, (SetOutput{b.bus, c.bus}));
    // Output after output.
    CHECK(refused(h, SetOutput{c.bus, a.bus},
                  "bus \"C\" to bus \"A\" would make a feedback loop: \"A\" -> \"B\" -> \"C\" "
                  "already"));
    // A send that closes a loop over outputs.
    CHECK(refused(h, add_send(ids, c.bus, 0, a.bus), "would make a feedback loop"));
    // An output that closes a loop over a send.
    History h2(new_session(settings()));
    Ids ids2(h2.current());
    const AddBus x = add_bus(ids2, "X");
    const AddBus y = add_bus(ids2, "Y");
    CHECK_APPLY(h2, x);
    CHECK_APPLY(h2, y);
    CHECK_APPLY(h2, add_send(ids2, x.bus, 0, y.bus));
    CHECK(refused(h2, SetOutput{y.bus, x.bus}, "\"X\" -> \"Y\" already"));
    // Rerouting the edge a path runs over is not a loop: A -> B -> C, and A
    // moving to C leaves B where it was.
    CHECK_APPLY(h, (SetOutput{a.bus, c.bus}));
    CHECK(invariants_hold(h.current()));

    // check_invariants catches a loop no operation could make (a file).
    Snapshot s = h.current();
    auto edit = std::make_shared<EditState>(*s.edit);
    edit->buses[2].output = b.bus;   // C -> B -> C
    s.edit = edit;
    std::string why;
    CHECK(!check_invariants(s, why));
    CHECK(mentions(why, "feedback loop"));
    return 0;
}

int test_refusals() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddBus verb = add_bus(ids, "Verb");
    CHECK_APPLY(h, verb);
    const AddBus sub = add_bus(ids, "Sub");
    CHECK_APPLY(h, sub);

    CHECK(refused(h, SetOutput{vox.track, 9999}, "neither the Master nor an aux bus"));
    CHECK(refused(h, SetOutput{9999, verb.bus}, "neither a track nor an aux bus"));
    AddTrack midi = add_track(ids, "Keys", 0);
    midi.kind = TrackKind::Midi;
    midi.mix.input.clear();
    CHECK_APPLY(h, midi);
    CHECK(refused(h, SetOutput{midi.track, verb.bus}, "MIDI track"));

    // A bus that is an output stays until nothing plays into it.
    CHECK_APPLY(h, (SetOutput{vox.track, verb.bus}));
    CHECK(refused(h, RemoveBus{verb.bus},
                  "bus \"Verb\" is the output of track \"Vox\"; route the track elsewhere"));
    CHECK_APPLY(h, (SetOutput{vox.track, kMasterOutput}));
    CHECK_APPLY(h, (SetOutput{sub.bus, verb.bus}));
    CHECK(refused(h, RemoveBus{verb.bus}, "is the output of bus \"Sub\""));

    // Rerouting and removing in one compound is one undo step.
    Compound both;
    both.ops.push_back(SetOutput{sub.bus, kMasterOutput});
    both.ops.push_back(RemoveBus{verb.bus});
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, both);
    CHECK(h.undo_depth() == depth + 1);
    CHECK(h.undo());
    CHECK(output_of_bus(h.current(), 1) == verb.bus);

    // check_invariants: an output naming nothing, and a MIDI track's.
    Snapshot s = h.current();
    auto edit = std::make_shared<EditState>(*s.edit);
    edit->buses[0].output = 424242;
    s.edit = edit;
    std::string why;
    CHECK(!check_invariants(s, why) && mentions(why, "buses[0].output"));
    s = h.current();
    edit = std::make_shared<EditState>(*s.edit);
    auto keys = std::make_shared<TrackEdit>(*edit->tracks[1]);
    keys->output = sub.bus;
    edit->tracks[1] = keys;
    s.edit = edit;
    CHECK(!check_invariants(s, why) && mentions(why, "a MIDI track has no audio output"));
    return 0;
}

// ADR-0024 §4: aux solo is mix state, saved and never undone.
int test_aux_solo_is_mix_state() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddBus verb = add_bus(ids, "Verb");
    CHECK_APPLY(h, verb);
    const std::size_t depth = h.undo_depth();
    BusMix m = *h.current().mix.find_bus(verb.bus);
    CHECK(!m.solo);
    m.solo = true;
    CHECK(!is_undoable(SetBusMix{m}));
    CHECK_APPLY(h, (SetBusMix{m}));
    CHECK(h.undo_depth() == depth);
    CHECK(h.current().mix.find_bus(verb.bus)->solo);
    return 0;
}

int test_codec_window_and_file() {
    for (const Operation& op : {Operation{SetOutput{3, 7}}, Operation{SetOutput{3, kMasterOutput}},
                                Operation{SetBusMix{BusMix{7, 0.0f, 0.0f, false, true}}}}) {
        std::string bytes;
        std::string error;
        CHECK(encode_operation(op, bytes, error));
        Operation back;
        CHECK(decode_operation(bytes, back, error));
        std::string again;
        CHECK(encode_operation(back, again, error));
        CHECK(again == bytes);
    }

    Document doc(new_session(settings()), 4096);
    Replica replica;
    ReplicationUpdate u;
    std::string error;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));
    Ids ids(doc.current());
    const AddTrack vox = add_track(ids, "Vox", 2);
    CHECK(doc.apply(vox, u, error) && replica.receive(u, error));
    const AddBus sub = add_bus(ids, "Sub");
    CHECK(doc.apply(sub, u, error) && replica.receive(u, error));
    const AddBus top = add_bus(ids, "Top");
    CHECK(doc.apply(top, u, error) && replica.receive(u, error));
    CHECK(doc.apply(SetOutput{vox.track, sub.bus}, u, error) && replica.receive(u, error));
    CHECK(doc.apply(SetOutput{sub.bus, top.bus}, u, error) && replica.receive(u, error));
    BusMix solo = *doc.current().mix.find_bus(top.bus);
    solo.solo = true;
    CHECK(doc.apply(SetBusMix{solo}, u, error) && replica.receive(u, error));
    CHECK(replica.current() == doc.current());

    // A replica that starts late gets outputs in the undo window.
    CHECK(doc.undo(u, error) && replica.receive(u, error));
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(late.current().edit->tracks[0]->output == sub.bus);
    CHECK(late.current().mix.find_bus(top.bus)->solo);
    CHECK(doc.redo(u, error) && late.receive(u, error) && replica.receive(u, error));
    CHECK(late.current() == doc.current() && late.current().edit->buses[0].output == top.bus);

    std::string bytes;
    CHECK(serialize(doc.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(mentions(bytes, ("\"output\": " + std::to_string(sub.bus)).c_str()));
    CHECK(mentions(bytes, "\"solo\": true"));
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(l.file_version == kFormatVersion && kFormatVersion >= 12);
    CHECK(same_document(l.snapshot, doc.current()));
    CHECK(l.snapshot.edit->tracks[0]->output == sub.bus);
    CHECK(l.snapshot.edit->buses[0].output == top.bus);
    CHECK(l.snapshot.mix.find_bus(top.bus)->solo);
    std::string again;
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error));
    CHECK(again == bytes);

    // A file whose output names no bus is refused by key path.
    const std::string key = "\"output\": " + std::to_string(sub.bus);
    std::string broken = bytes;
    broken.replace(broken.find(key), key.size(), "\"output\": 99999");
    Loaded nope;
    CHECK(!deserialize(broken, nope, le));
    CHECK(mentions(le.text(), "neither the Master (0) nor an aux bus"));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_set_output_and_undo);
    t(test_feedback_through_outputs_is_refused);
    t(test_refusals);
    t(test_aux_solo_is_mix_state);
    t(test_codec_window_and_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_routing: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_routing: %d passed\n", run);
    return 0;
}
