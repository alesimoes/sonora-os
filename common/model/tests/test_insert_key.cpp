// SPDX-License-Identifier: GPL-3.0-or-later
//
// Side-chain keys (ADR-0058): SetInsertKey as one undo step, what it
// refuses (an unknown track, insert or bus, a second key on a chain, a key
// that closes a loop through the buses, named), the key edge joining the
// bus graph for later sends and outputs, RemoveBus taking its keys with it
// in the same step, a stale key accepted, and the key surviving the codec,
// the undo window and the session file (format 26; a 25 file opens unkeyed).
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

AddInsert add_insert(Ids& ids, Id track, std::uint8_t slot) {
    AddInsert op;
    op.track = track;
    op.insert = ids();
    op.slot = slot;
    op.plugin = PluginRef{PluginFormat::Lv2, "urn:sonora:test:gate", "Gate", "1"};
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
    return mentions(error, says) && h.current() == before;
}

const Insert& insert_of(const History& h, std::size_t track, std::size_t i) {
    return h.current().edit->tracks[track]->inserts[i];
}

// Kick (a gate in slot 0, an EQ in slot 1) and Snare, the aux buses Drums
// and Key.
struct Rig {
    History h{new_session(settings())};
    AddTrack kick, snare;
    AddInsert gate, eq;
    AddBus drums, key;
};

int build(Rig& r) {
    Ids ids(r.h.current());
    r.kick = add_track(ids, "Kick");
    CHECK_APPLY(r.h, r.kick);
    r.snare = add_track(ids, "Snare");
    CHECK_APPLY(r.h, r.snare);
    r.gate = add_insert(ids, r.kick.track, 0);
    CHECK_APPLY(r.h, r.gate);
    r.eq = add_insert(ids, r.kick.track, 1);
    CHECK_APPLY(r.h, r.eq);
    r.drums = add_bus(ids, "Drums");
    CHECK_APPLY(r.h, r.drums);
    r.key = add_bus(ids, "Key");
    CHECK_APPLY(r.h, r.key);
    return 0;
}

int test_key_undo_redo() {
    Rig r;
    if (build(r) != 0) return 1;
    const SetInsertKey key{r.kick.track, r.gate.insert, r.key.bus};
    CHECK(is_undoable(key));
    const std::size_t depth = r.h.undo_depth();
    CHECK_APPLY(r.h, key);
    CHECK(r.h.undo_depth() == depth + 1);
    CHECK(insert_of(r.h, 0, 0).key_bus == r.key.bus);
    CHECK(insert_of(r.h, 0, 1).key_bus == 0);
    CHECK(invariants_hold(r.h.current()));
    const Snapshot keyed = r.h.current();
    CHECK(r.h.undo());
    CHECK(insert_of(r.h, 0, 0).key_bus == 0);
    CHECK(r.h.redo());
    CHECK(r.h.current() == keyed);
    // Self-inverse: the same operation with 0 takes the key off.
    CHECK_APPLY(r.h, (SetInsertKey{r.kick.track, r.gate.insert, 0}));
    CHECK(insert_of(r.h, 0, 0).key_bus == 0);
    CHECK(r.h.undo());
    CHECK(r.h.current() == keyed);
    // A replacement plug-in starts unkeyed; undo brings the keyed one back.
    Ids ids(r.h.current());
    ReplaceInsert swap;
    swap.insert = r.gate.insert;
    swap.replacement = ids();
    swap.plugin = PluginRef{PluginFormat::Lv2, "urn:sonora:test:comp", "Comp", "1"};
    CHECK_APPLY(r.h, swap);
    CHECK(insert_of(r.h, 0, 0).key_bus == 0);
    CHECK(r.h.undo());
    CHECK(r.h.current() == keyed);
    return 0;
}

int test_refusals() {
    namespace why = insert_key_refusal;
    Rig r;
    if (build(r) != 0) return 1;
    CHECK(refused(r.h, SetInsertKey{9999, r.gate.insert, r.key.bus}, why::kUnknownTrack));
    CHECK(refused(r.h, SetInsertKey{r.snare.track, r.gate.insert, r.key.bus},
                  why::kUnknownInsert));
    CHECK(refused(r.h, SetInsertKey{r.kick.track, r.gate.insert, 9999}, why::kUnknownBus));
    CHECK(refused(r.h, SetInsertKey{r.kick.track, r.gate.insert, r.kick.track},
                  why::kUnknownBus));
    CHECK_APPLY(r.h, (SetInsertKey{r.kick.track, r.gate.insert, r.key.bus}));
    // The plug-in host keys a whole chain: one keyed insert on it.
    CHECK(refused(r.h, SetInsertKey{r.kick.track, r.eq.insert, r.drums.bus}, why::kSecondKey));
    // Re-keying the keyed insert itself is no second key.
    CHECK_APPLY(r.h, (SetInsertKey{r.kick.track, r.gate.insert, r.drums.bus}));
    return 0;
}

// The regression the key edge is for: track -> send -> bus -> key of a
// chain on that same track is a loop, refused by name, and so is any later
// send or output that would close one through an existing key.
int test_loops() {
    namespace why = insert_key_refusal;
    Rig r;
    if (build(r) != 0) return 1;
    Ids ids(r.h.current());
    // Kick sends to Drums, Drums feeds Key: keying Kick by Key loops.
    CHECK_APPLY(r.h, add_send(ids, r.kick.track, 0, r.drums.bus));
    CHECK_APPLY(r.h, (SetOutput{r.drums.bus, r.key.bus}));
    CHECK(refused(r.h, SetInsertKey{r.kick.track, r.gate.insert, r.key.bus}, why::kLoop));
    CHECK(refused(r.h, SetInsertKey{r.kick.track, r.gate.insert, r.key.bus},
                  "track \"Kick\" -> bus \"Drums\" -> bus \"Key\" -> key of track \"Kick\""));
    // Its own output bus as its key loops at once.
    CHECK(refused(r.h, SetInsertKey{r.kick.track, r.gate.insert, r.drums.bus}, why::kLoop));
    // Through the Master only is no loop: Snare keyed by Key, out to the Master.
    CHECK_APPLY(r.h, add_insert(ids, r.snare.track, 0));
    const Id snare_gate = r.h.current().edit->tracks[1]->inserts[0].id;
    CHECK_APPLY(r.h, (SetInsertKey{r.snare.track, snare_gate, r.key.bus}));
    CHECK(invariants_hold(r.h.current()));
    // Now the key is an edge: Snare may not reach Key again, by send or
    // output, nor may a bus Snare feeds be routed into Key.
    CHECK(refused(r.h, add_send(ids, r.snare.track, 0, r.key.bus), "feedback loop"));
    CHECK(refused(r.h, SetOutput{r.snare.track, r.drums.bus}, "feedback loop"));
    const AddBus fx = add_bus(ids, "FX");
    CHECK_APPLY(r.h, fx);
    CHECK_APPLY(r.h, add_send(ids, r.snare.track, 1, fx.bus));
    CHECK(refused(r.h, SetOutput{fx.bus, r.key.bus}, "feedback loop"));
    CHECK(refused(r.h, add_send(ids, fx.bus, 0, r.drums.bus), "feedback loop"));
    // Moving a keyed insert onto a chain that feeds its key loops too.
    CHECK(refused(r.h, MoveInsert{snare_gate, r.kick.track, 5}, "feedback loop"));
    return 0;
}

int test_remove_bus_takes_its_keys() {
    Rig r;
    if (build(r) != 0) return 1;
    Ids ids(r.h.current());
    CHECK_APPLY(r.h, add_insert(ids, r.snare.track, 0));
    const Id snare_gate = r.h.current().edit->tracks[1]->inserts[0].id;
    CHECK_APPLY(r.h, (SetInsertKey{r.kick.track, r.gate.insert, r.key.bus}));
    CHECK_APPLY(r.h, (SetInsertKey{r.snare.track, snare_gate, r.key.bus}));
    const Snapshot keyed = r.h.current();
    const std::size_t depth = r.h.undo_depth();
    CHECK_APPLY(r.h, RemoveBus{r.key.bus});
    CHECK(r.h.undo_depth() == depth + 1);
    CHECK(r.h.current().edit->buses.size() == 1);
    CHECK(insert_of(r.h, 0, 0).key_bus == 0 && insert_of(r.h, 1, 0).key_bus == 0);
    CHECK(invariants_hold(r.h.current()));
    CHECK(r.h.undo());
    CHECK(r.h.current() == keyed);
    CHECK(insert_of(r.h, 0, 0).key_bus == r.key.bus && insert_of(r.h, 1, 0).key_bus == r.key.bus);
    CHECK(r.h.redo());
    CHECK(insert_of(r.h, 0, 0).key_bus == 0);
    return 0;
}

int test_invariants() {
    Rig r;
    if (build(r) != 0) return 1;
    // A key naming no bus (an old file, a bus gone outside the model) is
    // kept and shown, never refused.
    Snapshot s = r.h.current();
    auto edit = std::make_shared<EditState>(*s.edit);
    auto kick = std::make_shared<TrackEdit>(*edit->tracks[0]);
    kick->inserts[0].key_bus = 9999;
    edit->tracks[0] = kick;
    s.edit = edit;
    CHECK(invariants_hold(s));
    // Two keyed inserts on one chain are not a session.
    kick->inserts[1].key_bus = r.key.bus;
    std::string error;
    CHECK(!check_invariants(s, error) && mentions(error, "one keyed insert"));
    // Nor is a loop through a key: Kick out to Key, keyed by Key.
    kick->inserts[1].key_bus = 0;
    kick->inserts[0].key_bus = r.key.bus;
    kick->output = r.key.bus;
    CHECK(!check_invariants(s, error) && mentions(error, "feedback loop"));
    return 0;
}

int test_codec_window_and_file() {
    const Operation op = SetInsertKey{2, 30, 60};
    std::string bytes, error;
    CHECK(encode_operation(op, bytes, error));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    const auto* k = back.get_if<SetInsertKey>();
    CHECK(k != nullptr && k->track == 2 && k->insert == 30 && k->key_bus == 60);

    Rig r;
    if (build(r) != 0) return 1;
    CHECK_APPLY(r.h, (SetInsertKey{r.kick.track, r.gate.insert, r.key.bus}));
    WindowParts parts;
    CHECK(encode_window(r.h, parts, error));
    std::optional<History> window;
    CHECK(decode_window(parts, window, error));
    CHECK(window && window->current() == r.h.current());

    std::string file;
    CHECK(serialize(r.h.current(), WriteInfo{"test", 1}, file, error));
    CHECK(mentions(file, "\"key_bus\": " ) && file.find("\"key_bus\"") == file.rfind("\"key_bus\""));
    Loaded l;
    LoadError le;
    CHECK(deserialize(file, l, le));
    CHECK(l.file_version == kFormatVersion && kFormatVersion == 26);
    CHECK(same_document(l.snapshot, r.h.current()));
    std::string again;
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error) && again == file);

    // A version 25 file holds no key: it opens unkeyed, and one that
    // claims the key is not a version 25 file.
    std::string old = file;
    old.replace(old.find("\"sonora_session\": 26"), 20, "\"sonora_session\": 25");
    Loaded nope;
    CHECK(!deserialize(old, nope, le));
    CHECK(mentions(le.text(), "version 25 does not define"));
    std::string unkeyed;
    CHECK(r.h.undo());
    CHECK(serialize(r.h.current(), WriteInfo{"test", 1}, unkeyed, error));
    CHECK(unkeyed.find("key_bus") == std::string::npos);
    unkeyed.replace(unkeyed.find("\"sonora_session\": 26"), 20, "\"sonora_session\": 25");
    Loaded v25;
    CHECK(deserialize(unkeyed, v25, le));
    CHECK(v25.snapshot.edit->tracks[0]->inserts[0].key_bus == 0);
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
    t(test_key_undo_redo);
    t(test_refusals);
    t(test_loops);
    t(test_remove_bus_takes_its_keys);
    t(test_invariants);
    t(test_codec_window_and_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_insert_key: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_insert_key: %d passed\n", run);
    return 0;
}
