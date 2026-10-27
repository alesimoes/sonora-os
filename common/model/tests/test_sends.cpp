// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sends and aux buses in the document (ADR-0022): the operations, what undo
// covers (structure yes, values no), feedback loops refused at the model,
// every other refusal, and sends surviving the operation codec, the session
// file and a replica.
#include <limits>
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

AddSend add_send(Ids& ids, Id strip, std::uint8_t slot, Id bus, float level_db = 0.0f) {
    AddSend op;
    op.strip = strip;
    op.send = ids();
    op.slot = slot;
    op.bus = bus;
    op.mix.level_db = level_db;
    return op;
}

bool refused(History& h, const Operation& op, const char* says) {
    const Snapshot before = h.current();
    std::string error;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", says);
        return false;
    }
    return mentions(error, says) && h.current() == before;
}

int test_add_remove_and_undo() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddBus verb = add_bus(ids, "Verb");
    CHECK_APPLY(h, verb);
    const AddSend b = add_send(ids, vox.track, 1, verb.bus, -6.0f);
    CHECK_APPLY(h, b);
    const AddBus delay = add_bus(ids, "Delay");
    CHECK_APPLY(h, delay);
    const AddSend a = add_send(ids, vox.track, 0, delay.bus);
    CHECK_APPLY(h, a);
    CHECK(invariants_hold(h.current()));

    // Sorted by slot, the values in the mix with their own id.
    const auto& sends = h.current().edit->tracks[0]->sends;
    CHECK(sends.size() == 2 && sends[0].id == a.send && sends[1].id == b.send);
    CHECK(sends[1].bus == verb.bus && sends[1].slot == 1);
    const SendMix* bm = h.current().mix.find_send(b.send);
    CHECK(bm != nullptr && bm->send == b.send && bm->level_db == -6.0f);
    CHECK(bm->follow_pan && !bm->pre && !bm->mute);
    CHECK(h.current().edit->buses.size() == 2 && h.current().edit->buses[0].name == "Verb");
    CHECK(h.current().mix.find_bus(verb.bus)->gain_db == 0.0f);

    // Every structural step is one undo step; undo takes the send away and
    // keeps its values, so redo brings it back as it was.
    CHECK(is_undoable(a) && is_undoable(verb) && h.undo_depth() == 5);
    CHECK_APPLY(h, (RemoveSend{b.send}));
    CHECK(h.current().edit->tracks[0]->sends.size() == 1);
    CHECK(h.current().mix.find_send(b.send) != nullptr);
    CHECK(h.undo());
    CHECK(h.current().edit->tracks[0]->sends.size() == 2);
    CHECK(h.undo());   // the second send
    CHECK(h.current().edit->tracks[0]->sends.size() == 1);
    CHECK(h.redo());
    CHECK(h.current().edit->tracks[0]->sends.size() == 2);
    CHECK(invariants_hold(h.current()));

    // A bus nothing feeds goes; its values stay for undo.
    CHECK_APPLY(h, (RemoveSend{a.send}));
    CHECK_APPLY(h, (RemoveBus{delay.bus}));
    CHECK(h.current().edit->buses.size() == 1);
    CHECK(h.current().mix.find_bus(delay.bus) != nullptr);
    CHECK(h.undo());
    CHECK(h.current().edit->buses.size() == 2);
    CHECK(invariants_hold(h.current()));
    return 0;
}

int test_values_are_mix_state() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddBus verb = add_bus(ids, "Verb");
    CHECK_APPLY(h, verb);
    const AddSend s = add_send(ids, vox.track, 0, verb.bus);
    CHECK_APPLY(h, s);
    const std::size_t depth = h.undo_depth();

    SendMix m = *h.current().mix.find_send(s.send);
    m.level_db = -12.0f;
    m.pan = -0.5f;
    m.follow_pan = false;
    m.mute = true;
    m.pre = true;
    const SetSendMix set{m};
    CHECK(!is_undoable(set));
    CHECK_APPLY(h, set);
    BusMix bm{verb.bus, -3.0f, 0.25f, true};
    CHECK(!is_undoable(SetBusMix{bm}));
    CHECK_APPLY(h, (SetBusMix{bm}));
    CHECK(h.undo_depth() == depth);
    CHECK(*h.current().mix.find_send(s.send) == m);
    CHECK(*h.current().mix.find_bus(verb.bus) == bm);
    // Undo of the send leaves the values where the fader left them.
    CHECK(h.undo());
    CHECK(*h.current().mix.find_send(s.send) == m);

    // A send that is gone is not edited; out-of-range values are refused.
    CHECK(refused(h, SetSendMix{m}, "is not on any track or aux bus"));
    CHECK(h.redo());
    SendMix loud = m;
    loud.level_db = 30.0f;
    CHECK(refused(h, SetSendMix{loud}, "level_db"));
    SendMix wide = m;
    wide.pan = 1.5f;
    CHECK(refused(h, SetSendMix{wide}, "pan"));
    BusMix nan_pan = bm;
    nan_pan.pan = std::numeric_limits<float>::quiet_NaN();
    CHECK(refused(h, SetBusMix{nan_pan}, "pan"));
    CHECK(refused(h, SetBusMix{BusMix{9999, 0.0f, 0.0f, false}}, "is not an aux bus"));
    return 0;
}

// ADR-0022 §4: a send that would close a loop through the buses is refused,
// naming the path, and so is a document that holds one.
int test_feedback_is_refused() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddBus a = add_bus(ids, "A");
    const AddBus b = add_bus(ids, "B");
    const AddBus c = add_bus(ids, "C");
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);
    CHECK_APPLY(h, c);
    CHECK(refused(h, add_send(ids, a.bus, 0, a.bus), "a bus cannot send to itself"));
    CHECK_APPLY(h, add_send(ids, a.bus, 0, b.bus));
    CHECK_APPLY(h, add_send(ids, b.bus, 0, c.bus));
    CHECK(refused(h, add_send(ids, c.bus, 0, a.bus),
                  "would make a feedback loop: \"A\" -> \"B\" -> \"C\" already"));
    CHECK(refused(h, add_send(ids, b.bus, 1, a.bus), "would make a feedback loop"));
    // Not a loop: a second path to the same bus.
    CHECK_APPLY(h, add_send(ids, a.bus, 1, c.bus));
    CHECK(invariants_hold(h.current()));

    // In a compound too: each member sees what the one before it made.
    Compound two;
    two.ops.push_back(add_send(ids, c.bus, 2, b.bus));
    CHECK(refused(h, two, "operation 1 of 1 (AddSend)"));

    // check_invariants catches a loop no operation could make (a file).
    Snapshot s = h.current();
    auto edit = std::make_shared<EditState>(*s.edit);
    edit->buses[2].sends.push_back(Send{s.next_id, 0, a.bus});
    s.edit = edit;
    s.mix.sends.push_back(SendMix{s.next_id});
    ++s.next_id;
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
    const AddBus other = add_bus(ids, "Other");
    CHECK_APPLY(h, other);
    const AddSend s = add_send(ids, vox.track, 2, verb.bus);
    CHECK_APPLY(h, s);

    CHECK(refused(h, add_send(ids, vox.track, kSendSlots, other.bus), "send slots (A to J)"));
    CHECK(refused(h, add_send(ids, vox.track, 2, other.bus), "slot C"));
    CHECK(refused(h, add_send(ids, vox.track, 3, verb.bus), "already sends to bus \"Verb\""));
    CHECK(refused(h, add_send(ids, vox.track, 3, 9999), "is not an aux bus"));
    CHECK(refused(h, add_send(ids, 9999, 3, verb.bus), "neither a track nor an aux bus"));
    AddSend stale = add_send(ids, vox.track, 3, other.bus);
    stale.send = s.send;
    CHECK(refused(h, stale, "already taken"));
    AddSend loud = add_send(ids, vox.track, 3, other.bus, 99.0f);
    CHECK(refused(h, loud, "mix.level_db"));
    CHECK(refused(h, RemoveSend{9999}, "is not on any track or aux bus"));
    CHECK(refused(h, RemoveBus{verb.bus}, "is fed by a send on track \"Vox\""));
    CHECK(refused(h, RemoveBus{9999}, "is not an aux bus"));
    AddBus bad = add_bus(ids, std::string("\xff", 1));
    CHECK(refused(h, bad, "not valid UTF-8"));

    // A MIDI track has no audio to send.
    AddTrack midi = add_track(ids, "Keys", 0);
    midi.kind = TrackKind::Midi;
    midi.mix.input.clear();
    CHECK_APPLY(h, midi);
    CHECK(refused(h, add_send(ids, midi.track, 0, verb.bus), "MIDI track"));

    // Removing the send and its bus in one compound is one undo step.
    Compound both;
    both.ops.push_back(RemoveSend{s.send});
    both.ops.push_back(RemoveBus{verb.bus});
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, both);
    CHECK(h.undo_depth() == depth + 1);
    CHECK(h.undo());
    CHECK(h.current().edit->buses.size() == 2);
    CHECK(h.current().edit->tracks[0]->sends.size() == 1);
    return 0;
}

int test_codec_replica_and_file() {
    // Every new operation crosses the codec unchanged.
    const std::vector<Operation> ops = {
        AddBus{7, "Verb", BusMix{0, -2.0f, 0.5f, true}},
        RemoveBus{7},
        AddSend{3, 8, 9, 7, SendMix{0, -10.0f, -0.25f, false, true, true}},
        RemoveSend{8},
        SetSendMix{SendMix{8, -1.5f, 0.75f, true, false, true}},
        SetBusMix{BusMix{7, 3.0f, -1.0f, false}},
    };
    for (const Operation& op : ops) {
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
    const AddBus verb = add_bus(ids, "Verb");
    CHECK(doc.apply(verb, u, error) && replica.receive(u, error));
    const AddBus room = add_bus(ids, "Room");
    CHECK(doc.apply(room, u, error) && replica.receive(u, error));
    const AddSend s = add_send(ids, vox.track, 4, verb.bus, -8.0f);
    CHECK(doc.apply(s, u, error) && replica.receive(u, error));
    const AddSend chain = add_send(ids, verb.bus, 9, room.bus, -20.0f);
    CHECK(doc.apply(chain, u, error) && replica.receive(u, error));
    SendMix pre = *doc.current().mix.find_send(s.send);
    pre.pre = true;
    CHECK(doc.apply(SetSendMix{pre}, u, error) && replica.receive(u, error));
    CHECK(doc.apply(RemoveSend{chain.send}, u, error) && replica.receive(u, error));
    CHECK(doc.undo(u, error) && replica.receive(u, error));
    CHECK(replica.current() == doc.current());

    // A replica that starts late gets sends and buses in the undo window,
    // and can redo across them.
    CHECK(doc.undo(u, error) && replica.receive(u, error));
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(doc.redo(u, error) && late.receive(u, error) && replica.receive(u, error));
    CHECK(late.current() == doc.current() && replica.current() == doc.current());

    // The file holds structure and values in one object per send, and the
    // buses after the tracks.
    std::string bytes;
    CHECK(serialize(doc.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(mentions(bytes, "\"buses\": ["));
    CHECK(mentions(bytes, "\"pre\": true"));
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(l.file_version == kFormatVersion);
    CHECK(same_document(l.snapshot, doc.current()));
    CHECK(l.snapshot.edit->buses.size() == 2);
    CHECK(l.snapshot.edit->buses[0].sends.size() == 1);
    CHECK(l.snapshot.mix.find_send(s.send)->pre);
    std::string again;
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error));
    CHECK(again == bytes);

    // A file whose send names no bus is refused by key path.
    const std::string bus_key = "\"bus\": " + std::to_string(verb.bus);
    std::string broken = bytes;
    broken.replace(broken.find(bus_key), bus_key.size(), "\"bus\": 99999");
    Loaded nope;
    CHECK(!deserialize(broken, nope, le));
    CHECK(mentions(le.text(), "is not an aux bus"));
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
    t(test_add_remove_and_undo);
    t(test_values_are_mix_state);
    t(test_feedback_is_refused);
    t(test_refusals);
    t(test_codec_replica_and_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_sends: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_sends: %d passed\n", run);
    return 0;
}
