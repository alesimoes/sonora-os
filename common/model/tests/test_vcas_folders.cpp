// SPDX-License-Identifier: GPL-3.0-or-later
//
// VCA masters and folders (ADR-0042): the operations, what is undoable and
// what is not, a deleted track leaving its VCA and folder (and coming back
// with undo), the refusals, and the codec, window and session file.
#include <optional>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

struct Rig {
    History h{new_session(settings())};
    AddTrack a, b, c;
};

int build(Rig& r) {
    Ids ids(r.h.current());
    r.a = add_track(ids, "Kick");
    r.b = add_track(ids, "Snare");
    r.c = add_track(ids, "Bass");
    CHECK_APPLY(r.h, r.a);
    CHECK_APPLY(r.h, r.b);
    CHECK_APPLY(r.h, r.c);
    return 0;
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

int test_vca_structure_is_undoable_and_its_fader_is_not() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id vca = r.h.current().next_id;
    CHECK_APPLY(r.h, (SetVca{vca, "Drums", {r.a.track, r.b.track}}));
    CHECK(r.h.current().edit->vcas.size() == 1);
    CHECK(r.h.current().next_id == vca + 1);
    // The fader is a mix value: it moves, and undo does not take it back.
    const std::size_t depth = r.h.undo_depth();
    CHECK_APPLY(r.h, (SetVcaMix{VcaMix{vca, -6.0f, false}}));
    CHECK(r.h.undo_depth() == depth);
    CHECK(r.h.current().mix.find_vca(vca)->gain_db == -6.0f);
    // Replace: a third member, the same id.
    CHECK_APPLY(r.h, (SetVca{vca, "Drums", {r.a.track, r.b.track, r.c.track}}));
    CHECK(r.h.current().edit->vcas.size() == 1 && r.h.current().edit->vcas[0].members.size() == 3);
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->vcas[0].members.size() == 2);
    CHECK(r.h.current().mix.find_vca(vca)->gain_db == -6.0f);
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->vcas.empty());
    CHECK(r.h.redo());
    CHECK_APPLY(r.h, (RemoveVca{vca}));
    CHECK(r.h.current().edit->vcas.empty());
    return 0;
}

int test_vca_refusals() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id next = r.h.current().next_id;
    CHECK(refused(r.h, SetVca{next, "X", {9999}}, "does not exist"));
    CHECK(refused(r.h, SetVca{next, "X", {r.a.track, r.a.track}}, "listed twice"));
    CHECK(refused(r.h, SetVca{r.a.track, "X", {r.a.track}}, "already taken"));
    CHECK(refused(r.h, SetVca{next, "\xff", {r.a.track}}, "UTF-8"));
    CHECK(refused(r.h, RemoveVca{next}, "does not exist"));
    CHECK(refused(r.h, SetVcaMix{VcaMix{next, 0.0f, false}}, "does not exist"));
    CHECK_APPLY(r.h, (SetVca{next, "V", {}}));
    CHECK(refused(r.h, SetVcaMix{VcaMix{next, 1000.0f, false}}, "gain_db"));
    // A track on two VCAs is allowed: their dB add.
    CHECK_APPLY(r.h, (SetVca{next + 1, "W", {r.a.track}}));
    CHECK_APPLY(r.h, (SetVca{next, "V", {r.a.track}}));
    while (r.h.current().edit->vcas.size() < kMaxVcas) {
        CHECK_APPLY(r.h, (SetVca{r.h.current().next_id, "n", {}}));
    }
    CHECK(refused(r.h, SetVca{r.h.current().next_id, "one more", {}}, "at most"));
    return 0;
}

int test_folder_rules() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id folder = r.h.current().next_id;
    CHECK_APPLY(r.h, (SetFolder{folder, "Drums", {r.a.track, r.b.track}}));
    const Id other = r.h.current().next_id;
    CHECK(refused(r.h, SetFolder{other, "Two", {r.b.track, r.c.track}}, "two folders"));
    CHECK_APPLY(r.h, (SetFolder{other, "Two", {r.c.track}}));
    // Moving a track from one folder to another is two replacements in one
    // compound, one undo step.
    Compound move;
    move.label = "Move";
    move.ops.push_back(SetFolder{folder, "Drums", {r.a.track}});
    move.ops.push_back(SetFolder{other, "Two", {r.c.track, r.b.track}});
    CHECK_APPLY(r.h, move);
    CHECK(r.h.current().edit->folders[1].members.size() == 2);
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->folders[0].members.size() == 2);
    CHECK_APPLY(r.h, (RemoveFolder{folder}));
    CHECK(r.h.current().edit->folders.size() == 1);
    CHECK(refused(r.h, RemoveFolder{folder}, "does not exist"));
    return 0;
}

int test_deleting_a_track_leaves_and_undo_returns() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id vca = r.h.current().next_id;
    CHECK_APPLY(r.h, (SetVca{vca, "V", {r.a.track, r.b.track}}));
    const Id folder = r.h.current().next_id;
    CHECK_APPLY(r.h, (SetFolder{folder, "F", {r.a.track, r.c.track}}));
    CHECK_APPLY(r.h, (RemoveTrack{r.a.track}));
    CHECK(r.h.current().edit->vcas[0].members == std::vector<Id>{r.b.track});
    CHECK(r.h.current().edit->folders[0].members == std::vector<Id>{r.c.track});
    CHECK(r.h.undo());
    CHECK((r.h.current().edit->vcas[0].members == std::vector<Id>{r.a.track, r.b.track}));
    CHECK((r.h.current().edit->folders[0].members == std::vector<Id>{r.a.track, r.c.track}));
    return 0;
}

int test_codec_window_and_file() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id vca = r.h.current().next_id;
    CHECK_APPLY(r.h, (SetVca{vca, "Drums", {r.a.track, r.b.track}}));
    CHECK_APPLY(r.h, (SetVcaMix{VcaMix{vca, -3.5f, true}}));
    const Id folder = r.h.current().next_id;
    CHECK_APPLY(r.h, (SetFolder{folder, "Rhythm", {r.b.track, r.c.track}}));

    const Operation ops[] = {Operation{SetVca{vca + 9, "V", {r.a.track}}}, Operation{RemoveVca{vca}},
                             Operation{SetVcaMix{VcaMix{vca, -1.0f, true}}},
                             Operation{SetFolder{folder + 9, "F", {r.c.track}}},
                             Operation{RemoveFolder{folder}}};
    for (const Operation& op : ops) {
        std::string bytes, error;
        CHECK(encode_operation(op, bytes, error));
        Operation back;
        CHECK(decode_operation(bytes, back, error));
        CHECK(std::string(operation_name(op)) == std::string(operation_name(back)));
        std::string again;
        CHECK(encode_operation(back, again, error) && again == bytes);
    }
    std::string error;
    Operation out;
    CHECK(!decode_operation(R"({"op": "SetVca", "id": 5, "name": "x"})", out, error));

    WindowParts w;
    CHECK(encode_window(r.h, w, error));
    std::optional<History> replica;
    CHECK(decode_window(w, replica, error));
    CHECK(replica.has_value() && replica->current() == r.h.current());
    CHECK(replica->current().mix.find_vca(vca)->gain_db == -3.5f);

    std::string bytes;
    CHECK(serialize(r.h.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(mentions(bytes, "\"vcas\"") && mentions(bytes, "\"folders\""));
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(l.snapshot.edit->vcas == r.h.current().edit->vcas);
    CHECK(l.snapshot.edit->folders == r.h.current().edit->folders);
    CHECK(l.snapshot.mix.vcas == r.h.current().mix.vcas);
    std::string again;
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error) && again == bytes);

    // A file that names a track that is not there is refused.
    std::string broken = bytes;
    const std::size_t at = broken.find("\"members\"");
    CHECK(at != std::string::npos);
    const std::size_t open = broken.find('[', at);
    broken.insert(open + 1, "9999, ");
    Loaded bad;
    CHECK(!deserialize(broken, bad, le));
    // A version 18 file cannot carry the keys.
    std::string v18 = bytes;
    const std::size_t ver = v18.find("\"sonora_session\": 19");
    CHECK(ver != std::string::npos);
    v18.replace(ver, 20, "\"sonora_session\": 18");
    Loaded old;
    CHECK(!deserialize(v18, old, le));
    CHECK(mentions(le.message, "version 18 does not define"));
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
    t(test_vca_structure_is_undoable_and_its_fader_is_not);
    t(test_vca_refusals);
    t(test_folder_rules);
    t(test_deleting_a_track_leaves_and_undo_returns);
    t(test_codec_window_and_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_vcas_folders: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_vcas_folders: %d passed\n", run);
    return 0;
}
