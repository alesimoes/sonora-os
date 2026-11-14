// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware output paths (ADR-0060): SetBusPath as an undo step, the pairs it
// refuses, a path that cannot be routed on, the Master kept off a path's
// pair, and the path surviving the codec, the undo window and the session
// file (format 25).
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

int test_path_and_undo() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack kick = add_track(ids, "Kick");
    CHECK_APPLY(h, kick);
    const AddBus cue = add_bus(ids, "Cue");
    CHECK_APPLY(h, cue);
    CHECK(h.current().edit->buses[0].hardware == kNoHardwareOut);

    const SetBusPath path{cue.bus, "Headphones 1", 2};
    CHECK(is_undoable(path));
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, path);
    CHECK(h.undo_depth() == depth + 1);
    CHECK(h.current().edit->buses[0].hardware == 2);
    CHECK(h.current().edit->buses[0].name == "Headphones 1");
    CHECK_APPLY(h, (SetOutput{kick.track, cue.bus}));
    CHECK(invariants_hold(h.current()));

    CHECK(h.undo());
    CHECK(h.undo());
    CHECK(h.current().edit->buses[0].hardware == kNoHardwareOut);
    CHECK(h.current().edit->buses[0].name == "Cue");
    CHECK(h.redo());
    CHECK(h.current().edit->buses[0].hardware == 2);

    // Back to an ordinary aux bus, keeping the new name.
    CHECK_APPLY(h, (SetBusPath{cue.bus, "Cue", kNoHardwareOut}));
    CHECK(h.current().edit->buses[0].hardware == kNoHardwareOut);
    return 0;
}

int test_refusals() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddBus a = add_bus(ids, "A");
    const AddBus b = add_bus(ids, "B");
    const AddBus c = add_bus(ids, "C");
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);
    CHECK_APPLY(h, c);
    // The Master plays on outputs 1 and 2 until it is told otherwise.
    CHECK(refused(h, SetBusPath{a.bus, "A", 0}, "the Master plays on outputs 1 and 2"));
    CHECK(refused(h, SetBusPath{a.bus, "A", 3}, "from an odd number"));
    CHECK(refused(h, SetBusPath{a.bus, "A", kMaxOutputChannels}, "from an odd number"));
    CHECK(refused(h, SetBusPath{a.bus, "", 2}, "1 to 64 characters"));
    CHECK(refused(h, SetBusPath{9999, "A", 2}, "is not an aux bus"));
    CHECK_APPLY(h, (SetBusPath{a.bus, "Out 3-4", 2}));
    CHECK(refused(h, SetBusPath{b.bus, "Again", 2}, "already carry the path \"Out 3-4\""));
    // The Master cannot be moved onto a path's pair.
    SetMonitorBus onto;
    onto.bus = h.current().mix.bus;
    onto.bus.outputs = {{3, 4}};
    CHECK(refused(h, onto, "carry the path \"Out 3-4\""));
    // A path ends at the interface.
    CHECK(refused(h, SetOutput{a.bus, b.bus}, "is an output path"));
    // A bus that feeds another leaves it when it becomes a path.
    CHECK_APPLY(h, (SetOutput{c.bus, b.bus}));
    CHECK_APPLY(h, (SetBusPath{c.bus, "Out 5-6", 4}));
    CHECK(h.current().edit->buses[2].output == kMasterOutput);
    return 0;
}

int test_codec_window_and_file() {
    const Operation op = SetBusPath{7, "Wedge L/R", 8};
    std::string bytes;
    std::string error;
    CHECK(encode_operation(op, bytes, error));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    CHECK(back.get_if<SetBusPath>()->first == 8);
    CHECK(back.get_if<SetBusPath>()->name == "Wedge L/R");
    CHECK(back.get_if<SetBusPath>()->bus == 7);

    Document doc(new_session(settings()), 4096);
    Replica replica;
    ReplicationUpdate u;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));
    Ids ids(doc.current());
    CHECK(doc.apply(add_bus(ids, "Cue"), u, error) && replica.receive(u, error));
    CHECK(doc.apply(SetBusPath{doc.current().edit->buses[0].id, "Cue", 2}, u, error) &&
          replica.receive(u, error));
    CHECK(replica.current() == doc.current());
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current().edit->buses[0].hardware == 2);

    std::string file;
    CHECK(serialize(doc.current(), WriteInfo{"test", 1}, file, error));
    CHECK(mentions(file, "\"hardware\": 2"));
    Loaded l;
    LoadError le;
    CHECK(deserialize(file, l, le));
    CHECK(l.file_version == kFormatVersion && kFormatVersion == 26);
    CHECK(same_document(l.snapshot, doc.current()));
    CHECK(l.snapshot.edit->buses[0].hardware == 2);
    std::string again;
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error));
    CHECK(again == file);

    // An odd pair start is refused by key path.
    std::string odd = file;
    odd.replace(odd.find("\"hardware\": 2"), 13, "\"hardware\": 3");
    Loaded nope;
    CHECK(!deserialize(odd, nope, le));
    CHECK(mentions(le.text(), "buses[0].hardware"));

    // A file that says version 24 and holds the key is not a version 24 file.
    std::string old = file;
    old.replace(old.find("\"sonora_session\": 26"), 20, "\"sonora_session\": 24");
    CHECK(!deserialize(old, nope, le));
    CHECK(mentions(le.text(), "version 24 does not define"));
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
    t(test_path_and_undo);
    t(test_refusals);
    t(test_codec_window_and_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_output_paths: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_output_paths: %d passed\n", run);
    return 0;
}
