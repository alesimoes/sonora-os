// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix and edit groups (ADR-0041): SetGroup and RemoveGroup, what undo
// brings back, a deleted track leaving its group, what a move on one member
// does to the others (relative and absolute), and the codec and the session
// file carrying groups.
#include <cmath>
#include <memory>

#include "model_test.h"
#include "sonora/model/groups.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

struct Rig {
    History h{new_session(settings())};
    Id t[4] = {0, 0, 0, 0};
    Id group = 0;
};

int build(Rig& r) {
    Ids ids(r.h.current());
    for (int i = 0; i < 4; ++i) {
        AddTrack a = add_track(ids, "T" + std::to_string(i + 1));
        r.t[i] = a.track;
        CHECK_APPLY(r.h, a);
    }
    return 0;
}

SetGroup make_group(Id id, std::vector<Id> members, std::uint8_t attrs = kGroupVolume | kGroupMute) {
    SetGroup g;
    g.group = id;
    g.name = "Drums";
    g.members = std::move(members);
    g.attrs = attrs;
    return g;
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

int test_create_undo_remove() {
    Rig r;
    if (build(r) != 0) return 1;
    r.group = r.h.current().next_id;
    CHECK_APPLY(r.h, make_group(r.group, {r.t[0], r.t[1]}));
    CHECK(r.h.current().edit->groups.size() == 1);
    CHECK(r.h.current().next_id == r.group + 1);
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->groups.empty());
    CHECK(r.h.redo());
    CHECK(r.h.current().edit->groups.size() == 1);
    // The same id replaces: the name and the members change, one step.
    SetGroup edit = make_group(r.group, {r.t[0], r.t[1], r.t[2]}, kGroupSolo);
    edit.name = "Kit";
    edit.relative = false;
    CHECK_APPLY(r.h, edit);
    const Group& g = r.h.current().edit->groups[0];
    CHECK(g.name == "Kit" && g.members.size() == 3 && g.attrs == kGroupSolo && !g.relative);
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->groups[0].name == "Drums");
    CHECK_APPLY(r.h, (RemoveGroup{r.group}));
    CHECK(r.h.current().edit->groups.empty());
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->groups.size() == 1);
    return 0;
}

int test_refusals() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id id = r.h.current().next_id;
    CHECK(refused(r.h, make_group(id, {r.t[0]}), "at least two"));
    CHECK(refused(r.h, make_group(id, {r.t[0], 99999}), "does not exist"));
    CHECK(refused(r.h, make_group(id, {r.t[0], r.t[0]}), "two groups"));
    CHECK(refused(r.h, make_group(id, {r.t[0], r.t[1]}, 0), "attrs"));
    CHECK(refused(r.h, make_group(id, {r.t[0], r.t[1]}, 64), "attrs"));
    CHECK(refused(r.h, make_group(1, {r.t[0], r.t[1]}), "already taken"));
    CHECK(refused(r.h, RemoveGroup{id}, "does not exist"));
    CHECK_APPLY(r.h, make_group(id, {r.t[0], r.t[1]}));
    // A track is in one group at most.
    CHECK(refused(r.h, make_group(id + 1, {r.t[1], r.t[2]}), "two groups"));
    return 0;
}

int test_delete_track_leaves_group() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id id = r.h.current().next_id;
    CHECK_APPLY(r.h, make_group(id, {r.t[0], r.t[1], r.t[2]}));
    CHECK_APPLY(r.h, (RemoveTrack{r.t[2]}));
    CHECK(r.h.current().edit->groups[0].members.size() == 2);
    // Down to one track, the group goes; undo brings both back.
    CHECK_APPLY(r.h, (RemoveTrack{r.t[1]}));
    CHECK(r.h.current().edit->groups.empty());
    CHECK(r.h.undo());
    CHECK(r.h.current().edit->groups.size() == 1);
    CHECK(r.h.current().edit->groups[0].members.size() == 2);
    return 0;
}

TrackMix mix_of(const Snapshot& s, Id t) { return *s.mix.find(t); }

int test_follow() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id id = r.h.current().next_id;
    CHECK_APPLY(r.h, make_group(id, {r.t[0], r.t[1], r.t[2]}, kGroupVolume | kGroupMute));
    TrackMix a = mix_of(r.h.current(), r.t[0]);
    TrackMix b = mix_of(r.h.current(), r.t[1]);
    b.gain_db = -10.0f;
    CHECK_APPLY(r.h, SetMix{b});
    // Relative: the leader up 3 dB lifts the others by 3 dB.
    a.gain_db = 3.0f;
    auto peers = group_follow(r.h.current(), a);
    CHECK(peers.size() == 2);
    for (const TrackMix& m : peers) {
        CHECK(m.track != r.t[3]);
        CHECK(std::fabs(m.gain_db - (m.track == r.t[1] ? -7.0f : 3.0f)) < 1e-4f);
    }
    // Clamped, never past the fader's end.
    a.gain_db = 100.0f;
    for (const TrackMix& m : group_follow(r.h.current(), a)) CHECK(m.gain_db == kMaxGainDb);
    // Mute is copied; solo is not linked by this group.
    a = mix_of(r.h.current(), r.t[0]);
    a.mute = true;
    a.solo = true;
    peers = group_follow(r.h.current(), a);
    CHECK(peers.size() == 2);
    for (const TrackMix& m : peers) CHECK(m.mute && !m.solo);
    // Absolute: every peer goes to the leader's value.
    SetGroup abs = make_group(id, {r.t[0], r.t[1], r.t[2]}, kGroupVolume);
    abs.relative = false;
    CHECK_APPLY(r.h, abs);
    a = mix_of(r.h.current(), r.t[0]);
    a.gain_db = -20.0f;
    for (const TrackMix& m : group_follow(r.h.current(), a)) CHECK(m.gain_db == -20.0f);
    // Suspended, or a track outside the group, links nothing.
    SetGroup off = abs;
    off.active = false;
    CHECK_APPLY(r.h, off);
    CHECK(group_follow(r.h.current(), a).empty());
    CHECK(group_peers(*r.h.current().edit, r.t[0], kGroupVolume).empty());
    off.active = true;
    CHECK_APPLY(r.h, off);
    TrackMix out = mix_of(r.h.current(), r.t[3]);
    out.gain_db = 5.0f;
    CHECK(group_follow(r.h.current(), out).empty());
    CHECK(group_peers(*r.h.current().edit, r.t[0], kGroupVolume).size() == 2);
    CHECK(group_peers(*r.h.current().edit, r.t[0], kGroupArm).empty());
    // The follow set goes in as one compound: one revision, every member.
    a.gain_db = -20.0f;
    Compound c;
    c.ops.push_back(SetMix{a});
    for (const TrackMix& m : group_follow(r.h.current(), a)) c.ops.push_back(SetMix{m});
    const std::uint64_t rev = r.h.current().revision;
    CHECK_APPLY(r.h, c);
    CHECK(r.h.current().revision == rev + 1);
    CHECK(mix_of(r.h.current(), r.t[2]).gain_db == -20.0f);
    return 0;
}

int test_codec_and_file() {
    Rig r;
    if (build(r) != 0) return 1;
    const Id id = r.h.current().next_id;
    SetGroup g = make_group(id, {r.t[0], r.t[2]}, kGroupAllAttributes);
    g.relative = false;
    g.active = false;
    std::string bytes, error;
    CHECK(encode_operation(g, bytes, error));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    const auto* sg = get_if<SetGroup>(&back);
    CHECK(sg != nullptr && sg->group == id && sg->members == g.members &&
          sg->attrs == kGroupAllAttributes && !sg->relative && !sg->active && sg->name == "Drums");
    CHECK(encode_operation(RemoveGroup{id}, bytes, error));
    CHECK(decode_operation(bytes, back, error) && holds_alternative<RemoveGroup>(back));
    CHECK(!decode_operation(R"({"op":"SetGroup","id":1})", back, error));

    CHECK_APPLY(r.h, g);
    std::string file;
    CHECK(serialize(r.h.current(), WriteInfo{"test", 1}, file, error));
    CHECK(file.find("\"groups\"") != std::string::npos);
    Loaded loaded;
    LoadError le;
    CHECK(deserialize(file, loaded, le));
    CHECK(loaded.file_version == kFormatVersion);
    CHECK(loaded.snapshot.edit->groups == r.h.current().edit->groups);
    CHECK(loaded.snapshot.next_id > id);
    std::string again;
    CHECK(serialize(loaded.snapshot, WriteInfo{"test", 1}, again, error) && again == file);
    // A group naming a track the file does not have is refused.
    std::string bad = file;
    const std::size_t at = bad.find("\"members\": [");
    CHECK(at != std::string::npos);
    bad.insert(at + 12, "9999, ");
    CHECK(!deserialize(bad, loaded, le));
    // A version 18 file may not hold groups.
    std::string old = file;
    const std::size_t v = old.find("\"sonora_session\": 24");
    CHECK(v != std::string::npos);
    old.replace(v, 20, "\"sonora_session\": 18");
    CHECK(!deserialize(old, loaded, le) && le.message.find("groups") != std::string::npos);
    // The undo window carries groups and the next id.
    WindowParts parts;
    CHECK(encode_window(r.h, parts, error));
    std::optional<History> window;
    CHECK(decode_window(parts, window, error));
    CHECK(window && window->current() == r.h.current());
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
    t(test_create_undo_remove);
    t(test_refusals);
    t(test_delete_track_leaves_group);
    t(test_follow);
    t(test_codec_and_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_groups: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_groups: %d passed\n", run);
    return 0;
}
