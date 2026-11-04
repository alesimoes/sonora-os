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
#include "sonora/model/session_template.h"

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

    // The mode, every one of them (Trim and WriteTrim since format 16).
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
    for (const AutomationMode mode : {AutomationMode::Trim, AutomationMode::WriteTrim}) {
        m.automation = mode;
        CHECK_APPLY(h, SetMix{m});
        CHECK(h.current().mix.find(vox.track)->automation == mode);
    }
    m.automation = static_cast<AutomationMode>(7);
    CHECK(refused(h, SetMix{m}, "is not an automation mode"));
    return 0;
}

// Trim (ADR-0027 §8 slice 5): a relative dB lane in [-60, +12], after the
// other lanes; it names no insert; it crosses the codec and the undo
// window, as do the trim modes.
int test_volume_trim_lane_and_modes() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    CHECK_APPLY(h, whole(vox.track, AutomationParam::VolumeTrim, {{0, -3.0f}, {48000, 6.0f}}));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {{0, -6.0f}}));
    const auto& lanes = h.current().edit->tracks[0]->automation;
    CHECK(lanes.size() == 2 && lanes[0]->param == AutomationParam::Volume &&
          lanes[1]->param == AutomationParam::VolumeTrim);
    CHECK(lane_of(h.current(), 0, AutomationParam::VolumeTrim)->points[1].value == 6.0f);
    CHECK(invariants_hold(h.current()));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::VolumeTrim, {{0, -60.0f}, {10, 12.0f}}));
    CHECK(refused(h, whole(vox.track, AutomationParam::VolumeTrim, {{0, 12.5f}}),
                  "a trim point must be from -60 to 12 dB"));
    CHECK(refused(h, whole(vox.track, AutomationParam::VolumeTrim, {{0, -61.0f}}),
                  "a trim point must be from -60 to 12 dB"));
    CHECK(refused(h,
                  whole(vox.track, AutomationParam::VolumeTrim,
                        {{0, std::numeric_limits<float>::quiet_NaN()}}),
                  "a trim point must be from -60 to 12 dB"));
    EditAutomation keyed = whole(vox.track, AutomationParam::VolumeTrim, {{0, 0.0f}});
    keyed.insert = 42;
    CHECK(refused(h, keyed, "names no insert and no key"));

    const Operation op = EditAutomation{
        vox.track, AutomationParam::VolumeTrim, 100, 5000, {{100, -0.5f}, {4000, 3.0f}}};
    std::string bytes;
    std::string error;
    CHECK(encode_operation(op, bytes, error));
    CHECK(mentions(bytes, "\"volume_trim\""));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    std::string again;
    CHECK(encode_operation(back, again, error));
    CHECK(again == bytes);
    const auto* e = std::get_if<EditAutomation>(&back);
    CHECK(e != nullptr && e->param == AutomationParam::VolumeTrim && e->points.size() == 2);

    for (const AutomationMode mode : {AutomationMode::Trim, AutomationMode::WriteTrim}) {
        TrackMix m = *h.current().mix.find(vox.track);
        m.automation = mode;
        const Operation set = SetMix{m};
        CHECK(encode_operation(set, bytes, error));
        CHECK(mentions(bytes, mode == AutomationMode::Trim ? "\"trim\"" : "\"write_trim\""));
        CHECK(decode_operation(bytes, back, error));
        const auto* s = std::get_if<SetMix>(&back);
        CHECK(s != nullptr && s->mix.automation == mode);
        CHECK(encode_operation(back, again, error) && again == bytes);
    }

    // The undo window (format 13) carries the trim lane and mode.
    Document doc(new_session(settings()), 4096);
    ReplicationUpdate u;
    Ids dids(doc.current());
    const AddTrack bass = add_track(dids, "Bass");
    CHECK(doc.apply(bass, u, error));
    CHECK(doc.apply(whole(bass.track, AutomationParam::VolumeTrim, {{0, -9.0f}}), u, error));
    TrackMix m = *doc.current().mix.find(bass.track);
    m.automation = AutomationMode::WriteTrim;
    CHECK(doc.apply(SetMix{m}, u, error));
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(lane_of(late.current(), 0, AutomationParam::VolumeTrim)->points[0].value == -9.0f);
    CHECK(late.current().mix.find(bass.track)->automation == AutomationMode::WriteTrim);
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
    CHECK(mentions(bytes, "\"sonora_session\": 19"));
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

// --- mute, send and strip lanes (ADR-0027 §9) -------------------------------

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

EditAutomation send_whole(Id strip, AutomationParam param, Id send,
                          std::vector<AutomationPoint> points) {
    EditAutomation op{strip, param, 0, kMaxInteger, std::move(points)};
    op.send = send;
    return op;
}

const AutomationLane* send_lane(const AutomationLanes& lanes, AutomationParam param, Id send) {
    AutomationLane which;
    which.param = param;
    which.send = send;
    return find_lane(lanes, which);
}

// Mute is a switch: 0 or 1, changing only at a step, so read linearly it
// never plays between the two. An edit that would leave a ramp where it
// meets the points it keeps is refused as one with a ramp of its own.
int test_mute_lane_changes_only_at_a_step() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Volume, {{0, -6.0f}}));
    CHECK_APPLY(h, whole(vox.track, AutomationParam::Mute,
                         {{0, 0.0f}, {48000, 0.0f}, {48000, 1.0f}, {96000, 1.0f}, {96000, 0.0f}}));
    const TrackEdit& t = *h.current().edit->tracks[0];
    CHECK(t.automation.size() == 2 && t.automation[1]->param == AutomationParam::Mute);
    CHECK(t.lane(AutomationParam::Mute)->points.size() == 5);
    CHECK(invariants_hold(h.current()));

    CHECK(refused(h, whole(vox.track, AutomationParam::Mute, {{0, 0.0f}, {48000, 1.0f}}),
                  "a mute changes only at a step"));
    CHECK(refused(h, whole(vox.track, AutomationParam::Mute, {{0, 0.5f}}), "must be 0 or 1"));
    CHECK(refused(h, whole(vox.track, AutomationParam::Mute, {{0, -0.0f}, {10, 2.0f}}),
                  "must be 0 or 1"));
    // [60000, 70000) inside the muted span, unmuted: the kept point at 48000
    // (1) would run straight to 60000 (0).
    CHECK(refused(h, (EditAutomation{vox.track, AutomationParam::Mute, 60000, 70000,
                                     {{60000, 0.0f}}}),
                  "the lane after the edit: a mute changes only at a step"));
    // As a step on both sides it goes.
    CHECK_APPLY(h, (EditAutomation{vox.track, AutomationParam::Mute, 60000, 70000,
                                   {{60000, 1.0f}, {60000, 0.0f}, {69999, 0.0f}, {69999, 1.0f}}}));
    CHECK(h.current().edit->tracks[0]->lane(AutomationParam::Mute)->points.size() == 9);
    CHECK(invariants_hold(h.current()));
    // A mute lane names no insert and no send.
    EditAutomation keyed = whole(vox.track, AutomationParam::Mute, {{0, 1.0f}});
    keyed.send = 5;
    CHECK(refused(h, keyed, "names no send"));
    return 0;
}

// Send level and send pan lanes, keyed by send: they go with RemoveSend and
// come back with its undo, and a duplicate's lanes key the copy's sends.
int test_send_lanes_follow_their_send() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddBus verb = add_bus(ids, "Verb");
    CHECK_APPLY(h, verb);
    const AddBus delay = add_bus(ids, "Delay");
    CHECK_APPLY(h, delay);
    const AddSend a = add_send(ids, vox.track, 0, verb.bus);
    CHECK_APPLY(h, a);
    const AddSend b = add_send(ids, vox.track, 1, delay.bus);
    CHECK_APPLY(h, b);

    CHECK_APPLY(h, send_whole(vox.track, AutomationParam::SendLevel, b.send, {{0, -10.0f}}));
    CHECK_APPLY(h, send_whole(vox.track, AutomationParam::SendLevel, a.send, {{0, -3.0f}}));
    CHECK_APPLY(h, send_whole(vox.track, AutomationParam::SendPan, a.send, {{0, -1.0f}}));
    CHECK(invariants_hold(h.current()));
    const AutomationLanes& lanes = h.current().edit->tracks[0]->automation;
    CHECK(lanes.size() == 3);
    // By param, then send.
    CHECK(lanes[0]->send == a.send && lanes[1]->send == b.send &&
          lanes[2]->param == AutomationParam::SendPan);
    CHECK(h.current().edit->tracks[0]->lane(AutomationParam::SendLevel) == nullptr);

    CHECK(refused(h, send_whole(vox.track, AutomationParam::SendLevel, 0, {{0, 0.0f}}),
                  "is not a send on track"));
    CHECK(refused(h, send_whole(vox.track, AutomationParam::SendLevel, verb.bus, {{0, 0.0f}}),
                  "is not a send on track"));
    CHECK(refused(h, send_whole(vox.track, AutomationParam::SendLevel, a.send, {{0, 30.0f}}),
                  "dB"));
    CHECK(refused(h, send_whole(vox.track, AutomationParam::SendPan, a.send, {{0, 1.5f}}),
                  "from -1 to 1"));
    EditAutomation with_insert = send_whole(vox.track, AutomationParam::SendPan, a.send, {});
    with_insert.insert = 4;
    CHECK(refused(h, with_insert, "names no insert"));

    // Removed: its lanes go, the other send's stay; undo brings them back.
    CHECK_APPLY(h, RemoveSend{a.send});
    CHECK(invariants_hold(h.current()));
    CHECK(h.current().edit->tracks[0]->automation.size() == 1);
    CHECK(send_lane(h.current().edit->tracks[0]->automation, AutomationParam::SendLevel,
                    b.send) != nullptr);
    CHECK(h.undo());
    CHECK(h.current().edit->tracks[0]->automation.size() == 3);

    // Duplicated with its sends: the copy's lanes key the copy's sends.
    DuplicateTrack dup;
    dup.source = vox.track;
    dup.name = "Vox.dup1";
    dup.track = h.current().next_id;
    CHECK_APPLY(h, dup);
    CHECK(invariants_hold(h.current()));
    const TrackEdit& copy = *h.current().edit->tracks[1];
    CHECK(copy.sends.size() == 2 && copy.automation.size() == 3);
    const Id a_copy = copy.sends[0].id;
    CHECK(a_copy != a.send);
    CHECK(send_lane(copy.automation, AutomationParam::SendPan, a_copy) != nullptr);
    CHECK(send_lane(copy.automation, AutomationParam::SendLevel, copy.sends[1].id) != nullptr);
    CHECK(send_lane(copy.automation, AutomationParam::SendLevel, a.send) == nullptr);
    CHECK(h.undo());
    // Without its sends: none to key, so no send lane.
    dup.sends = false;
    dup.track = h.current().next_id;
    CHECK_APPLY(h, dup);
    CHECK(invariants_hold(h.current()));
    CHECK(h.current().edit->tracks[1]->automation.empty());
    return 0;
}

// Lanes on an aux return and on the Master, each the params its strip has:
// `track` names the strip, kMasterOutput the Master.
int test_lanes_on_aux_returns_and_the_master() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddBus verb = add_bus(ids, "Verb");
    CHECK_APPLY(h, verb);
    const AddBus delay = add_bus(ids, "Delay");
    CHECK_APPLY(h, delay);
    const AddSend s = add_send(ids, verb.bus, 0, delay.bus);
    CHECK_APPLY(h, s);

    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, whole(verb.bus, AutomationParam::Volume, {{0, -6.0f}, {48000, 0.0f}}));
    CHECK(h.undo_depth() == depth + 1);
    CHECK_APPLY(h, whole(verb.bus, AutomationParam::Pan, {{0, 0.5f}}));
    CHECK_APPLY(h, whole(verb.bus, AutomationParam::Mute, {{0, 1.0f}}));
    CHECK_APPLY(h, send_whole(verb.bus, AutomationParam::SendLevel, s.send, {{0, -12.0f}}));
    CHECK_APPLY(h, whole(kMasterOutput, AutomationParam::Volume, {{0, -1.0f}}));
    CHECK(invariants_hold(h.current()));
    const Bus& bus = h.current().edit->buses[0];
    CHECK(bus.automation.size() == 4 && bus.automation[0]->points.size() == 2);
    CHECK(h.current().edit->master_automation.size() == 1);
    CHECK(h.current().edit->tracks[0]->automation.empty());

    CHECK(refused(h, whole(verb.bus, AutomationParam::VolumeTrim, {{0, 0.0f}}),
                  "has no volume trim to automate"));
    EditAutomation plugin = whole(verb.bus, AutomationParam::Plugin, {{0, 0.0f}});
    plugin.insert = 1;
    plugin.key = "gain";
    CHECK(refused(h, plugin, "has no plugin parameter"));
    CHECK(refused(h, send_whole(delay.bus, AutomationParam::SendLevel, s.send, {{0, 0.0f}}),
                  "is not a send on aux bus"));
    for (const AutomationParam p : {AutomationParam::Pan, AutomationParam::Mute,
                                    AutomationParam::VolumeTrim}) {
        CHECK(refused(h, whole(kMasterOutput, p, {{0, 0.0f}}), "the Master has no"));
    }
    CHECK(refused(h, whole(9999, AutomationParam::Volume, {{0, 0.0f}}), "does not exist"));

    // Removing the send takes the bus's send lane; undo, and the bus's lanes
    // are back as they were, shared with the step before.
    const Snapshot full = h.current();
    CHECK_APPLY(h, RemoveSend{s.send});
    CHECK(h.current().edit->buses[0].automation.size() == 3);
    CHECK(h.undo());
    CHECK(*h.current().edit == *full.edit);
    // Clearing the Master's lane empties its list; undo restores it.
    CHECK_APPLY(h, whole(kMasterOutput, AutomationParam::Volume, {}));
    CHECK(h.current().edit->master_automation.empty());
    CHECK(h.undo());
    CHECK(h.current().edit->master_automation.size() == 1);
    // A template keeps the Master's lanes as it keeps the buses.
    CHECK(template_of(h.current(), TemplateOptions{}).edit->master_automation.size() == 1);
    return 0;
}

// The new lanes cross the operation codec, the undo window (a late replica)
// and the session file.
int test_strip_lanes_cross_the_wire_and_the_file() {
    EditAutomation op = send_whole(5, AutomationParam::SendPan, 9, {{0, 0.25f}});
    std::string bytes, error;
    CHECK(encode_operation(op, bytes, error));
    CHECK(mentions(bytes, "\"send_pan\"") && mentions(bytes, "\"send\": 9"));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    const auto* e = std::get_if<EditAutomation>(&back);
    CHECK(e != nullptr && e->param == AutomationParam::SendPan && e->send == 9 && e->track == 5);
    std::string again;
    CHECK(encode_operation(back, again, error) && again == bytes);
    // A send lane carries its send; any other lane does not.
    CHECK(!decode_operation(
        R"({"op": "EditAutomation", "track": 5, "param": "send_level", "range": [0, 10],)"
        R"( "points": [[0, 0.0]]})",
        back, error));
    CHECK(mentions(error, "missing required key \"send\""));
    CHECK(!decode_operation(
        R"({"op": "EditAutomation", "track": 5, "param": "mute", "send": 9, "range": [0, 10],)"
        R"( "points": [[0, 0.0]]})",
        back, error));
    CHECK(encode_operation(whole(0, AutomationParam::Mute, {{0, 1.0f}}), bytes, error));
    CHECK(mentions(bytes, "\"mute\"") && !mentions(bytes, "\"send\""));

    Document doc(new_session(settings()), 4096);
    Replica replica;
    ReplicationUpdate u;
    CHECK(doc.snapshot(u, error) && replica.receive(u, error));
    Ids ids(doc.current());
    const AddTrack vox = add_track(ids, "Vox");
    const AddBus verb = add_bus(ids, "Verb");
    const AddSend s = add_send(ids, vox.track, 0, verb.bus);
    for (const Operation& o :
         {Operation(vox), Operation(verb), Operation(s),
          Operation(send_whole(vox.track, AutomationParam::SendLevel, s.send, {{0, -6.0f}})),
          Operation(whole(vox.track, AutomationParam::Mute, {{0, 0.0f}, {100, 0.0f}, {100, 1.0f}})),
          Operation(whole(verb.bus, AutomationParam::Pan, {{0, -0.5f}})),
          Operation(whole(kMasterOutput, AutomationParam::Volume, {{0, -2.0f}})),
          Operation(whole(verb.bus, AutomationParam::Volume, {{0, -4.0f}}))}) {
        CHECK(doc.apply(o, u, error) && replica.receive(u, error));
    }
    CHECK(replica.current() == doc.current());
    Replica late;
    CHECK(doc.snapshot(u, error) && late.receive(u, error));
    CHECK(late.current() == doc.current());
    CHECK(late.current().edit->buses[0].automation.size() == 2);
    CHECK(late.current().edit->master_automation.size() == 1);
    // The step before the last holds the bus's pan lane alone, and shares
    // the Master's lane with the last: each lane crossed once.
    const History* window = late.history();
    CHECK(window != nullptr && window->undo_depth() >= 1);
    const std::size_t d = window->undo_depth();
    CHECK(window->step_edit(d - 1)->buses[0].automation.size() == 1);
    CHECK(window->step_edit(d - 1)->master_automation[0].get() ==
          window->step_edit(d)->master_automation[0].get());
    CHECK(window->step_edit(d - 1)->buses[0].automation[0].get() ==
          window->step_edit(d)->buses[0].automation[1].get());

    CHECK(serialize(doc.current(), WriteInfo{"test", 1}, bytes, error));
    CHECK(mentions(bytes, "\"sonora_session\": 19"));
    CHECK(mentions(bytes, "\"param\": \"send_level\",\n          \"send\": "));
    Loaded l;
    LoadError le;
    if (!deserialize(bytes, l, le)) std::fprintf(stderr, "  %s\n", le.text().c_str());
    CHECK(deserialize(bytes, l, le));
    CHECK(same_document(l.snapshot, doc.current()));
    CHECK(serialize(l.snapshot, WriteInfo{"test", 1}, again, error) && again == bytes);
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
    t(test_volume_trim_lane_and_modes);
    t(test_codec_window_and_duplicate);
    t(test_duplicate_without_automation);
    t(test_plugin_lane_keyed_by_insert_and_key);
    t(test_plugin_lane_refusals);
    t(test_plugin_lanes_follow_their_insert);
    t(test_plugin_lanes_cross_the_wire_and_the_file);
    t(test_mute_lane_changes_only_at_a_step);
    t(test_send_lanes_follow_their_send);
    t(test_lanes_on_aux_returns_and_the_master);
    t(test_strip_lanes_cross_the_wire_and_the_file);
    if (failed != 0) {
        std::fprintf(stderr, "model_automation: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_automation: %d passed\n", run);
    return 0;
}
