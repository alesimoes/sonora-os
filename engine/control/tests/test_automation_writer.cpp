// SPDX-License-Identifier: GPL-3.0-or-later
//
// A pass of live automation into the document (ADR-0027 §3): one thinned
// EditAutomation per written lane, the lane kept outside what was written
// (its old points in a gap, its old value on a run's edges, the static
// fader where it had none), a release ramped back to it, a loop's later
// turn over an earlier one, and the pass as one "Automation Write" step.
#include <cstdio>
#include <string>
#include <vector>

#include "model_test.h"
#include "sonora/automation_writer.h"
#include "sonora/model/history.h"

namespace {

using namespace model_test;
using sonora::automation_pass_edits;
using sonora::kAutomationWriteLabel;
using sonora::PassWrite;
using sonora::thin_automation;

// A held fader: one write per 100-frame block from `from` to `to`.
void ride(std::vector<PassWrite>& w, Id track, AutomationParam p, Frames from, Frames to,
          float value) {
    for (Frames f = from; f < to; f += 100) w.push_back({track, p, f, 100, value});
}

bool has(const std::vector<AutomationPoint>& pts, Frames at, float value) {
    for (const AutomationPoint& p : pts) {
        if (p.at == at && p.value == value) return true;
    }
    return false;
}

int test_thin() {
    std::vector<AutomationPoint> flat = {{0, 1.0f}, {100, 1.0f}, {200, 1.0f}, {300, 1.0f}};
    thin_automation(flat, 0.1f);
    CHECK(flat.size() == 2 && flat[0].at == 0 && flat[1].at == 300);
    // A line stays two points; a corner stays; a step keeps both its points.
    std::vector<AutomationPoint> corner = {{0, 0.0f},   {100, 1.0f},  {200, 2.0f},
                                           {300, 1.0f}, {300, -5.0f}, {400, -5.0f}};
    thin_automation(corner, 0.1f);
    CHECK(corner.size() == 5);
    CHECK(corner[1].at == 200 && corner[2].at == 300 && corner[3].at == 300);
    return 0;
}

int test_a_run_over_a_lane() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    // -20 dB, a point inside the coming gap at 5000, and one after.
    CHECK_APPLY(h, (EditAutomation{vox.track, AutomationParam::Volume, 0, kMaxInteger,
                                   {{0, -20.0f}, {5000, -10.0f}, {20000, -20.0f}}}));
    std::vector<PassWrite> w;
    ride(w, vox.track, AutomationParam::Volume, 1000, 3000, 0.0f);
    ride(w, vox.track, AutomationParam::Volume, 8000, 9000, -6.0f);
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w, 0);
    CHECK(edits.size() == 1);
    const EditAutomation& e = edits[0];
    CHECK(e.track == vox.track && e.param == AutomationParam::Volume);
    CHECK(e.from == 1000 && e.to == 9001);
    // Thinned: each held run is its edges.
    CHECK(e.points.size() == 9);
    // In at the old lane's value, a step to the fader, out the same way.
    CHECK(e.points[0].at == 1000 && e.points[0].value == -20.0f + 10.0f * 1000 / 5000);
    CHECK(e.points[1].at == 1000 && e.points[1].value == 0.0f);
    CHECK(has(e.points, 3000, 0.0f));
    // The gap's own point kept, between the runs.
    CHECK(has(e.points, 5000, -10.0f));
    CHECK(has(e.points, 8000, -6.0f) && has(e.points, 9000, -6.0f));
    CHECK(e.points.back().at == 9000);

    // Applied as the engine applies it: one step, labelled.
    Compound pass;
    pass.label = kAutomationWriteLabel;
    pass.ops.emplace_back(e);
    const std::size_t depth = h.undo_depth();
    CHECK_APPLY(h, pass);
    CHECK(h.undo_depth() == depth + 1);
    CHECK(h.undo_label() == "Automation Write");
    const AutomationLane* lane = h.current().edit->tracks[0]->lane(AutomationParam::Volume);
    CHECK(lane != nullptr && lane->points.front().at == 0 && lane->points.back().at == 20000);
    return 0;
}

int test_no_lane_keeps_the_static_fader() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    TrackMix m = *h.current().mix.find(vox.track);
    m.gain_db = -3.0f;
    m.pan = 0.5f;
    CHECK_APPLY(h, SetMix{m});
    std::vector<PassWrite> w;
    ride(w, vox.track, AutomationParam::Pan, 0, 500, -1.0f);
    ride(w, vox.track, AutomationParam::Volume, 200, 400, 6.0f);
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w, 0);
    CHECK(edits.size() == 2);
    // In the order first written.
    CHECK(edits[0].param == AutomationParam::Pan && edits[1].param == AutomationParam::Volume);
    CHECK(has(edits[0].points, 0, 0.5f) && has(edits[0].points, 0, -1.0f));
    CHECK(has(edits[0].points, 500, -1.0f) && has(edits[0].points, 500, 0.5f));
    CHECK(has(edits[1].points, 200, -3.0f) && has(edits[1].points, 400, -3.0f));
    // Unknown and MIDI tracks write nothing.
    std::vector<PassWrite> stray;
    ride(stray, 9999, AutomationParam::Volume, 0, 100, 0.0f);
    CHECK(automation_pass_edits(h.current(), stray, 0).empty());
    return 0;
}

int test_release_returns_and_later_turns_win() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    CHECK_APPLY(h, (EditAutomation{vox.track, AutomationParam::Volume, 0, kMaxInteger,
                                   {{0, -20.0f}}}));
    std::vector<PassWrite> w;
    // A loop over [0, 1000): first turn at -6 dB, the second at 0 dB from
    // 500, released at 800.
    ride(w, vox.track, AutomationParam::Volume, 0, 1000, -6.0f);
    ride(w, vox.track, AutomationParam::Volume, 500, 800, 0.0f);
    w.push_back({vox.track, AutomationParam::Volume, 800, 0, 0.0f});
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w, 100);
    CHECK(edits.size() == 1);
    const std::vector<AutomationPoint>& p = edits[0].points;
    CHECK(has(p, 0, -20.0f) && has(p, 0, -6.0f));
    CHECK(has(p, 500, 0.0f));
    // The return: from 0 dB at the release to the lane 100 frames on, then
    // the first turn's -6 dB to the end of the loop.
    CHECK(has(p, 800, 0.0f));
    CHECK(has(p, 900, -6.0f) || has(p, 900, -20.0f));
    CHECK(has(p, 1000, -6.0f) && has(p, 1000, -20.0f));
    for (std::size_t i = 1; i < p.size(); ++i) CHECK(p[i - 1].at <= p[i].at);
    CHECK_APPLY(h, edits[0]);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_thin();
    failed += test_a_run_over_a_lane();
    failed += test_no_lane_keeps_the_static_fader();
    failed += test_release_returns_and_later_turns_win();
    if (failed != 0) {
        std::fprintf(stderr, "automation_writer: %d failed\n", failed);
        return 1;
    }
    std::printf("automation_writer: ok\n");
    return 0;
}
