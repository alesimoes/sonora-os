// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation in the document (ADR-0027 §1): EditAutomation as an undo step
// that replaces a lane's points in a range, refusals, the mode as mix state
// outside undo, and lanes surviving the codec, the undo window and a
// duplicate. The session file's lanes: test_session_file.
#include <limits>
#include <memory>
#include <variant>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

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

EditAutomation whole(Id track, AutomationParam param, std::vector<AutomationPoint> points) {
    return EditAutomation{track, param, 0, kMaxInteger, std::move(points)};
}

const AutomationLane* lane_of(const Snapshot& s, std::size_t t, AutomationParam p) {
    return s.edit->tracks[t]->lane(p);
}

int test_edit_replaces_a_range_and_undoes() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume) == nullptr);

    const EditAutomation draw =
        whole(vox.track, AutomationParam::Volume, {{0, -6.0f}, {48000, 0.0f}, {96000, -12.0f}});
    CHECK(is_undoable(draw));
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, draw);
    CHECK(h.undo_depth() == depth + 1);
    const AutomationLane* vol = lane_of(h.current(), 0, AutomationParam::Volume);
    CHECK(vol != nullptr && vol->points.size() == 3);
    CHECK(invariants_hold(h.current()));

    // Pencil over [40000, 60000): the points outside stay, the one inside
    // goes, the new ones come in order.
    CHECK_APPLY(h, (EditAutomation{vox.track, AutomationParam::Volume, 40000, 60000,
                                   {{40000, -3.0f}, {50000, -3.0f}, {50000, 3.0f}}}));
    vol = lane_of(h.current(), 0, AutomationParam::Volume);
    CHECK(vol->points.size() == 5);
    CHECK(vol->points[0].at == 0 && vol->points[1].at == 40000 && vol->points[2].at == 50000);
    CHECK(vol->points[3].value == 3.0f && vol->points[4].at == 96000);

    // A pan lane goes after the volume lane, whatever order it came in.
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Pan, {{0, -1.0f}}));
    CHECK(h.current().edit->tracks[0]->automation.size() == 2);
    CHECK(h.current().edit->tracks[0]->automation[1]->param == AutomationParam::Pan);

    // The pan edit shares the volume lane with the step before it.
    const Snapshot after_pan = h.current();
    CHECK(h.undo());
    CHECK(after_pan.edit->tracks[0]->automation[0].get() ==
          h.current().edit->tracks[0]->automation[0].get());
    CHECK(lane_of(h.current(), 0, AutomationParam::Pan) == nullptr);
    CHECK(h.undo());
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume)->points.size() == 3);
    CHECK(h.redo());
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume)->points.size() == 5);

    // Clearing everything removes the lane.
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {}));
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume) == nullptr);
    CHECK(invariants_hold(h.current()));
    return 0;
}

int test_refusals() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    CHECK(refused(h, whole(vox.track + 100, AutomationParam::Volume, {{0, 0.0f}}),
                  "does not exist"));
    CHECK(refused(h, (EditAutomation{vox.track, AutomationParam::Volume, 10, 10, {}}),
                  "must run forward"));
    CHECK(refused(h, (EditAutomation{vox.track, AutomationParam::Volume, 100, 200, {{50, 0.0f}}}),
                  "outside the range"));
    CHECK(refused(h, whole(vox.track, AutomationParam::Volume, {{10, 0.0f}, {5, 0.0f}}),
                  "not sorted"));
    CHECK(refused(h,
                  whole(vox.track, AutomationParam::Volume, {{5, 0.0f}, {5, 1.0f}, {5, 2.0f}}),
                  "at most two points share a frame"));
    CHECK(refused(h, whole(vox.track, AutomationParam::Volume, {{0, 30.0f}}), "dB"));
    CHECK(refused(h, whole(vox.track, AutomationParam::Pan, {{0, 1.5f}}), "from -1 to 1"));
    std::vector<AutomationPoint> many(kMaxAutomationPoints + 1);
    for (std::size_t i = 0; i < many.size(); ++i) many[i].at = static_cast<Frames>(i);
    CHECK(refused(h, whole(vox.track, AutomationParam::Volume, many), "at most"));

    // A range is half open and ends at most at kMaxInteger, so a point at
    // kMaxInteger could never be replaced and its lane never emptied (#174
    // review). The last frame a point can sit on is one before.
    CHECK(refused(h, whole(vox.track, AutomationParam::Volume, {{kMaxInteger, 0.0f}}),
                  "2^53 - 2"));
    CHECK(refused(h,
                  (EditAutomation{vox.track, AutomationParam::Volume, kMaxInteger - 1,
                                  kMaxInteger, {{kMaxInteger, 0.0f}}}),
                  "2^53 - 2"));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume,
                         {{0, -6.0f}, {kMaxInteger - 1, 0.0f}, {kMaxInteger - 1, 3.0f}}));
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume)->points.size() == 3);
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {}));
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume) == nullptr);

    // The mode: every one but Trim, whose relative lane waits for its slice.
    TrackMix m = *h.current().mix.find(vox.track);
    CHECK(m.automation == AutomationMode::Read);
    m.automation = AutomationMode::Off;
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, SetMix{m});
    CHECK(h.undo_depth() == depth);   // mix state, outside undo
    CHECK(h.current().mix.find(vox.track)->automation == AutomationMode::Off);
    for (const AutomationMode mode :
         {AutomationMode::Touch, AutomationMode::Latch, AutomationMode::Write}) {
        m.automation = mode;
        CHECK_APPLY(h, SetMix{m});
        CHECK(h.current().mix.find(vox.track)->automation == mode);
    }
    m.automation = AutomationMode::Trim;
    CHECK(refused(h, SetMix{m}, "not available yet"));
    return 0;
}

int test_codec_window_and_duplicate() {
    const Operation op = EditAutomation{
        3, AutomationParam::Pan, 100, 5000, {{100, -0.5f}, {4000, 0.25f}, {4000, 1.0f}}};
    std::string bytes;
    std::string error;
    CHECK(encode_operation(op, bytes, error));
    CHECK(mentions(bytes, "\"EditAutomation\"") && mentions(bytes, "\"pan\""));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    std::string again;
    CHECK(encode_operation(back, again, error));
    CHECK(again == bytes);
    const auto* e = std::get_if<EditAutomation>(&back);
    CHECK(e != nullptr && e->from == 100 && e->to == 5000 && e->points.size() == 3);

    Document doc(new_session(settings()), 4096);
    Replica replica;
    ReplicationUpdate u;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));
    Ids ids(doc.current());
    const AddTrack vox = add_track(ids, "Vox", 2);
    CHECK(doc.apply(vox, u, error) && replica.receive(u, error));
    CHECK(doc.apply(whole(vox.track, AutomationParam::Volume, {{0, -6.0f}, {48000, 0.0f}}), u,
                    error) &&
          replica.receive(u, error));
    TrackMix m = *doc.current().mix.find(vox.track);
    m.automation = AutomationMode::Off;
    CHECK(doc.apply(SetMix{m}, u, error) && replica.receive(u, error));
    CHECK(replica.current() == doc.current());

    // A replica that starts late gets the lanes in the undo window.
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(lane_of(late.current(), 0, AutomationParam::Volume)->points.size() == 2);
    CHECK(late.current().mix.find(vox.track)->automation == AutomationMode::Off);

    // Pro Tools' Duplicate brings the automation with it.
    DuplicateTrack dup;
    dup.source = vox.track;
    dup.track = doc.current().next_id;
    dup.name = "Vox.dup1";
    CHECK(doc.apply(dup, u, error) && late.receive(u, error));
    CHECK(lane_of(late.current(), 1, AutomationParam::Volume) != nullptr);
    CHECK(late.current() == doc.current());
    return 0;
}

// Pro Tools' Duplicate Tracks dialog's "Automation" checkbox (ADR-0023,
// update of 2026-10-31): on by default; off, the copy has no lanes but keeps
// its source's mode, which is mix state.
int test_duplicate_without_automation() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {{0, -6.0f}, {48000, 0.0f}}));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Pan, {{0, -1.0f}}));
    TrackMix m = *h.current().mix.find(vox.track);
    m.automation = AutomationMode::Touch;
    CHECK_APPLY(h, SetMix{m});

    DuplicateTrack with;
    with.source = vox.track;
    with.name = "Vox.dup1";
    CHECK(with.automation);
    with.track = h.current().next_id;
    const std::size_t ids_with = duplicate_track_ids(h.current(), with);
    CHECK_APPLY(h, with);
    CHECK(h.current().edit->tracks[1]->automation.size() == 2);
    CHECK(h.current().edit->tracks[1]->automation[0].get() ==
          h.current().edit->tracks[0]->automation[0].get());
    CHECK(h.undo());

    DuplicateTrack without = with;
    without.track = h.current().next_id;
    without.automation = false;
    // Lanes take no ids.
    CHECK(duplicate_track_ids(h.current(), without) == ids_with);
    CHECK_APPLY(h, without);
    CHECK(invariants_hold(h.current()));
    CHECK(h.current().edit->tracks[1]->automation.empty());
    CHECK(lane_of(h.current(), 1, AutomationParam::Volume) == nullptr);
    CHECK(h.current().mix.find(without.track)->automation == AutomationMode::Touch);
    // The source keeps its lanes.
    CHECK(h.current().edit->tracks[0]->automation.size() == 2);

    // The flag crosses the wire, and an operation without it (a version-34
    // peer's) is refused rather than read as on.
    std::string bytes, error;
    CHECK(encode_operation(without, bytes, error));
    CHECK(mentions(bytes, "\"automation\""));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    const auto* d = std::get_if<DuplicateTrack>(&back);
    CHECK(d != nullptr && !d->automation);
    DuplicateTrack on = without;
    on.automation = true;
    CHECK(encode_operation(on, bytes, error) && decode_operation(bytes, back, error));
    d = std::get_if<DuplicateTrack>(&back);
    CHECK(d != nullptr && d->automation);

    const std::string v34 =
        R"({"op": "DuplicateTrack", "source": 1, "track": 9, "name": "Vox.dup1",)"
        R"( "all_playlists": false, "inserts": true, "sends": true})";
    CHECK(!decode_operation(v34, back, error));
    CHECK(mentions(error, "missing required key \"automation\""));
    return 0;
}

// --- plugin lanes (ADR-0027 §5, update of 2026-11-01) ----------------------

PluginRef limiter() {
    return PluginRef{PluginFormat::Lv2, "http://gareus.org/oss/lv2/dpl#mono",
                     "x42-dpl Digital Peak Limiter Mono", "0.9.2"};
}

PluginRef compressor() {
    return PluginRef{PluginFormat::Vst3, "565354436F6D703A6C73702D636F6D70",
                     "LSP Compressor Mono", "1.2.14"};
}

AddInsert add_insert(Ids& ids, Id track, std::uint8_t slot, PluginRef plugin = limiter()) {
    AddInsert op;
    op.track = track;
    op.insert = ids();
    op.slot = slot;
    op.plugin = std::move(plugin);
    return op;
}

EditAutomation plugin_whole(Id track, Id insert, const char* key,
                            std::vector<AutomationPoint> points) {
    EditAutomation op{track, AutomationParam::Plugin, 0, kMaxInteger, std::move(points)};
    op.insert = insert;
    op.key = key;
    return op;
}

const AutomationLane* plugin_lane(const Snapshot& s, std::size_t t, Id insert, const char* key) {
    return s.edit->tracks[t]->lane(insert, key);
}

int test_plugin_lane_keyed_by_insert_and_key() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddInsert lim = add_insert(ids, vox.track, 0);
    CHECK_APPLY(h, lim);
    const AddInsert comp = add_insert(ids, vox.track, 1, compressor());
    CHECK_APPLY(h, comp);

    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, plugin_whole(vox.track, comp.insert, "12", {{0, 0.25f}, {48000, 1.0f}}));
    CHECK(h.undo_depth() == depth + 1);
    CHECK_APPLY(h, plugin_whole(vox.track, lim.insert, "threshold", {{0, -6.0f}}));
    CHECK_APPLY(h, plugin_whole(vox.track, lim.insert, "release", {{0, 30.0f}}));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {{0, -3.0f}}));
    CHECK(invariants_hold(h.current()));

    // Sorted by (param, insert, key): volume, then the limiter's two in key
    // order, then the compressor's.
    const auto& lanes = h.current().edit->tracks[0]->automation;
    CHECK(lanes.size() == 4);
    CHECK(lanes[0]->param == AutomationParam::Volume && lanes[0]->insert == 0 &&
          lanes[0]->key.empty());
    CHECK(lanes[1]->insert == lim.insert && lanes[1]->key == "release");
    CHECK(lanes[2]->insert == lim.insert && lanes[2]->key == "threshold");
    CHECK(lanes[3]->insert == comp.insert && lanes[3]->key == "12");
    CHECK(plugin_lane(h.current(), 0, comp.insert, "12")->points.size() == 2);
    CHECK(plugin_lane(h.current(), 0, comp.insert, "13") == nullptr);
    // lane(param) names only the mix lanes; a plugin lane is not volume.
    CHECK(lane_of(h.current(), 0, AutomationParam::Pan) == nullptr);

    // Emptying one plugin lane leaves the others.
    CHECK_APPLY(h, plugin_whole(vox.track, lim.insert, "release", {}));
    CHECK(plugin_lane(h.current(), 0, lim.insert, "release") == nullptr);
    CHECK(plugin_lane(h.current(), 0, lim.insert, "threshold") != nullptr);
    CHECK(h.undo());
    CHECK(plugin_lane(h.current(), 0, lim.insert, "release") != nullptr);
    return 0;
}

int test_plugin_lane_refusals() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddTrack gtr = add_track(ids, "Gtr");
    CHECK_APPLY(h, gtr);
    const AddInsert lim = add_insert(ids, vox.track, 0);
    CHECK_APPLY(h, lim);
    const AddInsert comp = add_insert(ids, vox.track, 1, compressor());
    CHECK_APPLY(h, comp);

    CHECK(refused(h, plugin_whole(gtr.track, lim.insert, "threshold", {{0, 0.0f}}),
                  "is not an insert on track"));
    CHECK(refused(h, plugin_whole(vox.track, 0, "threshold", {{0, 0.0f}}),
                  "is not an insert on track"));
    CHECK(refused(h, plugin_whole(vox.track, lim.insert, "", {{0, 0.0f}}), "LV2 port symbol"));
    CHECK(refused(h, plugin_whole(vox.track, lim.insert, "9lives", {{0, 0.0f}}),
                  "LV2 port symbol"));
    CHECK(refused(h, plugin_whole(vox.track, comp.insert, "threshold", {{0, 0.0f}}),
                  "VST3 ParamID"));
    CHECK(refused(h, plugin_whole(vox.track, comp.insert, "12", {{0, 1.5f}}), "from 0 to 1"));
    CHECK(refused(h,
                  plugin_whole(vox.track, lim.insert, "threshold",
                               {{0, std::numeric_limits<float>::infinity()}}),
                  "finite"));
    // A mix lane names no insert and no key.
    EditAutomation vol = whole(vox.track, AutomationParam::Volume, {{0, 0.0f}});
    vol.insert = lim.insert;
    CHECK(refused(h, vol, "names no insert"));
    vol.insert = 0;
    vol.key = "threshold";
    CHECK(refused(h, vol, "names no insert"));
    // LV2 values are plain: any finite number, as ParamValue's are.
    CHECK_APPLY(h, plugin_whole(vox.track, lim.insert, "threshold", {{0, -40.0f}, {10, 900.0f}}));
    return 0;
}

// The lanes follow their insert: dropped with it, moved with it, renumbered
// with a duplicate's copy.
int test_plugin_lanes_follow_their_insert() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddTrack gtr = add_track(ids, "Gtr");
    CHECK_APPLY(h, gtr);
    const AddInsert lim = add_insert(ids, vox.track, 0);
    CHECK_APPLY(h, lim);
    const AddInsert comp = add_insert(ids, vox.track, 1, compressor());
    CHECK_APPLY(h, comp);
    CHECK_APPLY(h, plugin_whole(vox.track, lim.insert, "threshold", {{0, -6.0f}}));
    CHECK_APPLY(h, plugin_whole(vox.track, comp.insert, "12", {{0, 0.5f}}));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {{0, -3.0f}}));

    // Removed: its lanes go; undo brings them back.
    CHECK_APPLY(h, RemoveInsert{lim.insert});
    CHECK(invariants_hold(h.current()));
    CHECK(plugin_lane(h.current(), 0, lim.insert, "threshold") == nullptr);
    CHECK(plugin_lane(h.current(), 0, comp.insert, "12") != nullptr);
    CHECK(lane_of(h.current(), 0, AutomationParam::Volume) != nullptr);
    CHECK(h.undo());
    CHECK(plugin_lane(h.current(), 0, lim.insert, "threshold") != nullptr);

    // Replaced: another plugin, whose keys mean other things.
    ReplaceInsert rep;
    rep.insert = lim.insert;
    rep.replacement = h.current().next_id;
    rep.plugin = limiter();
    CHECK_APPLY(h, rep);
    CHECK(invariants_hold(h.current()));
    CHECK(plugin_lane(h.current(), 0, lim.insert, "threshold") == nullptr);
    CHECK(plugin_lane(h.current(), 0, rep.replacement, "threshold") == nullptr);
    CHECK(h.undo());

    // Moved to another track: its lanes go with it; the others stay.
    CHECK_APPLY(h, (MoveInsert{lim.insert, gtr.track, 3}));
    CHECK(invariants_hold(h.current()));
    CHECK(plugin_lane(h.current(), 0, lim.insert, "threshold") == nullptr);
    CHECK(plugin_lane(h.current(), 1, lim.insert, "threshold") != nullptr);
    CHECK(plugin_lane(h.current(), 0, comp.insert, "12") != nullptr);
    CHECK(lane_of(h.current(), 1, AutomationParam::Volume) == nullptr);
    // Within one track, nothing about the lanes changes.
    CHECK(h.undo());
    CHECK_APPLY(h, (MoveInsert{lim.insert, vox.track, 5}));
    CHECK(plugin_lane(h.current(), 0, lim.insert, "threshold") != nullptr);
    CHECK(h.undo());

    // Duplicated with its inserts: the copy's lanes key the copy's inserts.
    DuplicateTrack dup;
    dup.source = vox.track;
    dup.name = "Vox.dup1";
    dup.track = h.current().next_id;
    CHECK_APPLY(h, dup);
    CHECK(invariants_hold(h.current()));
    const TrackEdit& copy = *h.current().edit->tracks[1];
    CHECK(copy.inserts.size() == 2);
    const Id lim_copy = copy.inserts[0].id;
    const Id comp_copy = copy.inserts[1].id;
    CHECK(lim_copy != lim.insert && comp_copy != comp.insert);
    CHECK(copy.lane(lim_copy, "threshold") != nullptr);
    CHECK(copy.lane(comp_copy, "12") != nullptr);
    CHECK(copy.lane(lim.insert, "threshold") == nullptr);
    CHECK(copy.automation.size() == 3);
    CHECK(h.undo());

    // Duplicated without inserts: no insert to key, so only the mix lanes.
    dup.inserts = false;
    dup.track = h.current().next_id;
    CHECK_APPLY(h, dup);
    CHECK(invariants_hold(h.current()));
    CHECK(h.current().edit->tracks[1]->automation.size() == 1);
    CHECK(h.current().edit->tracks[1]->lane(AutomationParam::Volume) != nullptr);
    return 0;
}

int test_plugin_lanes_cross_the_wire_and_the_file() {
    EditAutomation op{3, AutomationParam::Plugin, 100, 5000, {{100, 0.5f}}};
    op.insert = 7;
    op.key = "threshold";
    std::string bytes, error;
    CHECK(encode_operation(op, bytes, error));
    CHECK(mentions(bytes, "\"plugin\"") && mentions(bytes, "\"insert\": 7") &&
          mentions(bytes, "\"key\": \"threshold\""));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    const auto* e = std::get_if<EditAutomation>(&back);
    CHECK(e != nullptr && e->param == AutomationParam::Plugin && e->insert == 7 &&
          e->key == "threshold");
    std::string again;
    CHECK(encode_operation(back, again, error) && again == bytes);
    // A volume edit carries neither key, so its encoding is what a
    // version-35 peer sends; a volume edit with one is not canonical.
    const Operation vol = whole(3, AutomationParam::Volume, {{0, 0.0f}});
    CHECK(encode_operation(vol, bytes, error));
    CHECK(!mentions(bytes, "\"insert\"") && !mentions(bytes, "\"key\""));
    CHECK(!decode_operation(
        R"({"op": "EditAutomation", "track": 3, "param": "volume", "insert": 7,)"
        R"( "key": "x", "range": [0, 10], "points": [[0, 0.0]]})",
        back, error));
    CHECK(!decode_operation(
        R"({"op": "EditAutomation", "track": 3, "param": "plugin", "range": [0, 10],)"
        R"( "points": [[0, 0.0]]})",
        back, error));

    Document doc(new_session(settings()), 4096);
    Replica replica;
    ReplicationUpdate u;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));
    Ids ids(doc.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK(doc.apply(vox, u, error) && replica.receive(u, error));
    const AddInsert lim = add_insert(ids, vox.track, 0);
    CHECK(doc.apply(lim, u, error) && replica.receive(u, error));
    CHECK(doc.apply(plugin_whole(vox.track, lim.insert, "threshold", {{0, -6.0f}, {48000, 0.0f}}),
                    u, error) &&
          replica.receive(u, error));
    CHECK(replica.current() == doc.current());
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(plugin_lane(late.current(), 0, lim.insert, "threshold")->points.size() == 2);

    CHECK(serialize(doc.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(mentions(bytes, "\"sonora_session\": 14"));
    CHECK(mentions(bytes, "\"param\": \"plugin\""));
    Loaded l;
    LoadError le;
    if (!deserialize(bytes, l, le)) std::fprintf(stderr, "  %s\n", le.text().c_str());
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, doc.current()));
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error) && again == bytes);

    // A lane naming an insert the track does not hold is refused by path.
    const std::string key = "\"insert\": " + std::to_string(lim.insert);
    std::string broken = bytes;
    const std::size_t at = broken.rfind(key);
    CHECK(at != std::string::npos);
    broken.replace(at, key.size(), "\"insert\": 99999");
    Loaded nope;
    CHECK(!deserialize(broken, nope, le));
    CHECK(mentions(le.text(), "automation[0]") && mentions(le.text(), "not an insert"));
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
    t(test_edit_replaces_a_range_and_undoes);
    t(test_refusals);
    t(test_codec_window_and_duplicate);
    t(test_duplicate_without_automation);
    t(test_plugin_lane_keyed_by_insert_and_key);
    t(test_plugin_lane_refusals);
    t(test_plugin_lanes_follow_their_insert);
    t(test_plugin_lanes_cross_the_wire_and_the_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_automation: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_automation: %d passed\n", run);
    return 0;
}
