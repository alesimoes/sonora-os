// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation in the document (ADR-0027 §1): EditAutomation as an undo step
// that replaces a lane's points in a range, refusals, the mode as mix state
// outside undo, and lanes surviving the codec, the undo window and a
// duplicate. The session file's lanes: test_session_file.
#include <memory>
#include <variant>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"

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
    if (failed != 0) {
        std::fprintf(stderr, "model_automation: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_automation: %d passed\n", run);
    return 0;
}
