// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plugin inserts in the document (ADR-0012 §6): the operations, what undo
// covers under owner decision O1's default (structure yes, values no),
// every refusal, and inserts surviving the session file and a replica.
#include <limits>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

PluginRef limiter() {
    return PluginRef{PluginFormat::Lv2, "http://gareus.org/oss/lv2/dpl#mono",
                     "x42-dpl Digital Peak Limiter Mono", "0.9.2"};
}

PluginRef compressor() {
    return PluginRef{PluginFormat::Vst3, "565354436F6D703A6C73702D636F6D70",
                     "LSP Compressor Mono", "1.2.14"};
}

StateRef state_of(char digit) {
    const std::string digest(64, digit);
    return StateRef{plugin_state_file(digest), digest, 812};
}

AddInsert add_insert(Ids& ids, Id track, std::uint8_t slot, PluginRef plugin = limiter()) {
    AddInsert op;
    op.track = track;
    op.insert = ids();
    op.slot = slot;
    op.plugin = std::move(plugin);
    if (op.plugin.format == PluginFormat::Lv2) op.params = {{"threshold", -1.0f}};
    return op;
}

const std::vector<Insert>& inserts_of(const Snapshot& s, std::size_t track) {
    return s.edit->tracks[track]->inserts;
}

bool refused(History& h, const Operation& op, const char* says) {
    const Snapshot before = h.current();
    std::string error;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", says);
        return false;
    }
    // A refusal changes nothing, revision included.
    return mentions(error, says) && h.current() == before;
}

int test_add_remove_and_undo() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    AddInsert a = add_insert(ids, vox.track, 3);
    a.state = state_of('a');
    CHECK_APPLY(h, a);
    const AddInsert b = add_insert(ids, vox.track, 0, compressor());
    CHECK_APPLY(h, b);
    CHECK(invariants_hold(h.current()));
    CHECK(h.current().next_id == b.insert + 1);

    // Sorted by slot whatever the order they were added in, and active.
    const auto& in = inserts_of(h.current(), 0);
    CHECK(in.size() == 2 && in[0].id == b.insert && in[1].id == a.insert);
    CHECK(in[0].slot == 0 && in[1].slot == 3 && in[0].active && in[1].active);
    CHECK(h.current().mix.find_insert(a.insert) != nullptr);
    const InsertMix am = *h.current().mix.find_insert(a.insert);
    CHECK(am.params == a.params && am.state == a.state && !am.bypass);

    // Structure is undoable: each add is one step.
    CHECK(is_undoable(a) && h.undo_depth() == 3);
    CHECK(h.undo());
    CHECK(inserts_of(h.current(), 0).size() == 1);
    // Its values outlive it, so redo brings it back as it was.
    CHECK(h.current().mix.find_insert(b.insert) != nullptr);
    CHECK(invariants_hold(h.current()));
    CHECK(h.redo());
    CHECK(inserts_of(h.current(), 0).size() == 2);

    CHECK_APPLY(h, RemoveInsert{a.insert});
    CHECK(inserts_of(h.current(), 0).size() == 1);
    CHECK(h.current().mix.find_insert(a.insert) != nullptr);
    CHECK(h.undo());
    CHECK(inserts_of(h.current(), 0).size() == 2);
    CHECK(*h.current().mix.find_insert(a.insert) == am);
    return 0;
}

int test_values_are_mix_state() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddInsert a = add_insert(ids, vox.track, 0);
    CHECK_APPLY(h, a);
    const std::size_t depth = h.undo_depth();

    CHECK_APPLY(h, (SetInsertBypass{a.insert, true}));
    CHECK_APPLY(h, (SetInsertParams{a.insert, {{"threshold", -3.0f}, {"release", 0.05f}}}));
    CHECK_APPLY(h, (SetInsertState{a.insert, state_of('c')}));
    // Not undoable: no step, and the edit state is the one it was.
    CHECK(!is_undoable(SetInsertBypass{}) && !is_undoable(SetInsertParams{}) &&
          !is_undoable(SetInsertState{}));
    CHECK(h.undo_depth() == depth);
    const InsertMix* m = h.current().mix.find_insert(a.insert);
    CHECK(m->bypass && m->params.size() == 2 && m->state == state_of('c'));
    CHECK_APPLY(h, (SetInsertState{a.insert, std::nullopt}));
    CHECK(!h.current().mix.find_insert(a.insert)->state);

    // Undo goes back past the add and leaves the values where they are.
    CHECK(h.undo());
    CHECK(inserts_of(h.current(), 0).empty());
    CHECK(h.current().mix.find_insert(a.insert)->bypass);
    // Values of an insert that is not on a track are not edited.
    CHECK(refused(h, SetInsertBypass{a.insert, false}, "is not on any track"));
    CHECK(h.redo());
    CHECK(h.current().mix.find_insert(a.insert)->bypass);
    return 0;
}

int test_move_active_replace() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    const AddTrack gtr = add_track(ids, "Gtr");
    const AddTrack keys = add_track(ids, "Keys", 2);
    CHECK_APPLY(h, vox);
    CHECK_APPLY(h, gtr);
    CHECK_APPLY(h, keys);
    const AddInsert a = add_insert(ids, vox.track, 0);
    const AddInsert b = add_insert(ids, vox.track, 1);
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, b);

    // Within a track: re-sorted by slot.
    CHECK_APPLY(h, (MoveInsert{a.insert, vox.track, 5}));
    CHECK(inserts_of(h.current(), 0)[0].id == b.insert);
    CHECK(inserts_of(h.current(), 0)[1].slot == 5);
    // Onto its own slot: allowed, nothing moves.
    CHECK_APPLY(h, (MoveInsert{a.insert, vox.track, 5}));
    // Onto a slot another insert holds: refused, naming the slot.
    CHECK(refused(h, MoveInsert{a.insert, vox.track, 1}, "slot B on track"));
    // Onto a track of another channel count: refused, since the plugin was
    // chosen for this one's and a chain has one layout (ADR-0012 §7.1).
    CHECK(refused(h, MoveInsert{a.insert, keys.track, 0}, "from a 1-channel track onto a 2-channel"));
    // To another track, keeping its values.
    CHECK_APPLY(h, (MoveInsert{a.insert, gtr.track, 9}));
    CHECK(inserts_of(h.current(), 0).size() == 1);
    CHECK(inserts_of(h.current(), 1).size() == 1 && inserts_of(h.current(), 1)[0].slot == 9);
    CHECK(h.current().mix.find_insert(a.insert)->params == a.params);
    CHECK(invariants_hold(h.current()));
    CHECK(h.undo());
    CHECK(inserts_of(h.current(), 1).empty() && inserts_of(h.current(), 0).size() == 2);

    // Inactive and back: undoable.
    CHECK_APPLY(h, (SetInsertActive{b.insert, false}));
    CHECK(!inserts_of(h.current(), 0)[0].active);
    CHECK(h.undo());
    CHECK(inserts_of(h.current(), 0)[0].active);
    CHECK_APPLY(h, (SetInsertActive{b.insert, false}));

    // Replace: a new id in the same slot, the old one's active state, new
    // values; undo brings back the old plugin with its own values.
    ReplaceInsert r;
    r.insert = b.insert;
    r.replacement = ids();
    r.plugin = compressor();
    r.params = {{"3", 0.75f}};
    CHECK_APPLY(h, r);
    const Insert& now = inserts_of(h.current(), 0)[0];
    CHECK(now.id == r.replacement && now.slot == 1 && !now.active && now.plugin == compressor());
    CHECK(h.current().mix.find_insert(r.replacement)->params == r.params);
    CHECK(h.current().next_id == r.replacement + 1);
    CHECK(h.undo());
    CHECK(inserts_of(h.current(), 0)[0].id == b.insert);
    CHECK(inserts_of(h.current(), 0)[0].plugin == limiter());
    CHECK(h.current().mix.find_insert(b.insert)->params == b.params);
    return 0;
}

int test_refusals() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddInsert a = add_insert(ids, vox.track, 0);
    CHECK_APPLY(h, a);

    AddInsert op = add_insert(ids, vox.track, 0);
    CHECK(refused(h, op, "slot A on track"));
    op.slot = 10;
    CHECK(refused(h, op, "not one of the 10 insert slots"));
    op.slot = 1;
    op.track = 999;
    CHECK(refused(h, op, "track 999 does not exist"));
    op.track = vox.track;
    op.insert = a.insert;
    CHECK(refused(h, op, "already taken"));
    op.insert = ids();
    op.plugin.id = "";
    CHECK(refused(h, op, "plugin.id: is empty"));
    op.plugin = compressor();
    op.plugin.id = "565354436f6d703a6c73702d636f6d70";
    op.params.clear();
    CHECK(refused(h, op, "32 upper-case hex digits"));
    op.plugin = compressor();
    op.params = {{"threshold", 0.5f}};
    CHECK(refused(h, op, "not a VST3 ParamID"));
    op.params = {{"12", 2.0f}};
    CHECK(refused(h, op, "normalised, from 0 to 1"));
    op.plugin = limiter();
    op.params = {{"threshold", std::numeric_limits<float>::quiet_NaN()}};
    CHECK(refused(h, op, "finite"));
    op.params = {{"threshold", 1.0f}, {"threshold", 2.0f}};
    CHECK(refused(h, op, "appears twice"));
    op.params = {};
    StateRef bad = state_of('d');
    bad.file = "../escape.state";
    op.state = bad;
    CHECK(refused(h, op, "file must be \"Plugin State/"));

    CHECK(refused(h, RemoveInsert{12345}, "insert 12345 is not on any track"));
    CHECK(refused(h, MoveInsert{a.insert, 999, 0}, "track 999 does not exist"));
    CHECK(refused(h, SetInsertParams{a.insert, {{"bad key", 1.0f}}}, "LV2 port symbol"));
    ReplaceInsert r;
    r.insert = a.insert;
    r.replacement = a.insert;
    r.plugin = compressor();
    CHECK(refused(h, r, "already taken"));
    return 0;
}

// What the window carries is what undo needs: a replica follows insert
// edits, undo and redo, and a snapshot taken mid-history restores both the
// structure and the values.
int test_replica_and_file() {
    Document doc(new_session(settings()));
    Replica replica;
    ReplicationUpdate u;
    std::string error;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));

    Ids ids(doc.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK(doc.apply(vox, u, error) && replica.receive(u, error));
    AddInsert a = add_insert(ids, vox.track, 2);
    a.state = state_of('e');
    CHECK(doc.apply(a, u, error) && replica.receive(u, error));
    CHECK(doc.apply(SetInsertBypass{a.insert, true}, u, error) && replica.receive(u, error));
    CHECK(doc.apply(RemoveInsert{a.insert}, u, error) && replica.receive(u, error));
    CHECK(doc.undo(u, error) && replica.receive(u, error));
    CHECK(replica.current() == doc.current());

    // A fresh replica from a snapshot whose window holds the removal as a
    // redo step: redo on it removes the insert, as on the document.
    CHECK(doc.snapshot(u, error));
    Replica late;
    CHECK(late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(late.can_redo());
    CHECK(doc.redo(u, error) && late.receive(u, error) && replica.receive(u, error));
    CHECK(inserts_of(late.current(), 0).empty());
    CHECK(late.current().mix.find_insert(a.insert)->bypass);
    CHECK(doc.undo(u, error) && late.receive(u, error));

    // The file holds the inserts there are, with their values, and not the
    // values left behind by an undone add.
    Ids more(doc.current());
    const AddInsert gone = add_insert(more, vox.track, 7);
    CHECK(doc.apply(gone, u, error));
    CHECK(doc.undo(u, error));
    std::string bytes;
    CHECK(serialize(doc.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(bytes.find("\"slot\": 2") != std::string::npos);
    CHECK(bytes.find("\"slot\": 7") == std::string::npos);
    Loaded l;
    LoadError le;
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, doc.current()));
    CHECK(l.snapshot.mix.inserts.size() == 1);
    CHECK(l.snapshot.mix.find_insert(a.insert)->state == a.state);
    return 0;
}

// An insert's values outlive it only while undo or redo can bring it back;
// then they are pruned, on the document and on every replica alike, so they
// stop riding every Snapshot window.
int test_orphaned_values_are_pruned() {
    // A new edit after an undo drops the redo branch, and with it the values
    // of an insert only that branch held.
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddInsert a = add_insert(ids, vox.track, 0);
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, (SetInsertParams{a.insert, {{"threshold", -3.0f}}}));
    CHECK(h.undo());
    CHECK(h.current().mix.find_insert(a.insert) != nullptr);   // redo still reaches it
    CHECK_APPLY(h, (SetTrackName{vox.track, "Lead"}));
    CHECK(h.current().mix.find_insert(a.insert) == nullptr);

    // A replaced insert's values go once every step that held it is below
    // the floor; the replica follows the floor and prunes the same.
    Document doc(new_session(settings()), 4096);
    Replica replica;
    ReplicationUpdate u;
    std::string error;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));
    Ids more(doc.current());
    const AddTrack gtr = add_track(more, "Gtr");
    CHECK(doc.apply(gtr, u, error) && replica.receive(u, error));
    const AddInsert b = add_insert(more, gtr.track, 0);
    CHECK(doc.apply(b, u, error) && replica.receive(u, error));
    ReplaceInsert r;
    r.insert = b.insert;
    r.replacement = more();
    r.plugin = limiter();
    CHECK(doc.apply(r, u, error) && replica.receive(u, error));
    CHECK(doc.current().mix.find_insert(b.insert) != nullptr);   // undo reaches it
    for (int i = 0; i < 64 && doc.current().mix.find_insert(b.insert) != nullptr; ++i) {
        AddMarker m;
        m.marker.id = more();
        m.marker.position = 1000 * i;
        m.marker.name = "M" + std::to_string(i);
        CHECK(doc.apply(m, u, error) && replica.receive(u, error));
    }
    CHECK(doc.current().mix.find_insert(b.insert) == nullptr);
    CHECK(doc.current().mix.find_insert(r.replacement) != nullptr);
    CHECK(replica.current() == doc.current());
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
    t(test_move_active_replace);
    t(test_refusals);
    t(test_replica_and_file);
    t(test_orphaned_values_are_pruned);
    if (failed != 0) {
        std::fprintf(stderr, "model_inserts: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_inserts: %d passed\n", run);
    return 0;
}
