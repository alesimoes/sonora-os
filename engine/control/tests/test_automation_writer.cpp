// SPDX-License-Identifier: GPL-3.0-or-later
//
// A pass of live automation into the document (ADR-0027 §3): one thinned
// EditAutomation per written lane, the lane kept outside what was written
// (its old points in a gap, its old value on a run's edges, the static
// fader where it had none), a release ramped back to it, a loop's later
// turn over an earlier one, and the pass as one "Automation Write" step.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "model_test.h"
#include "sonora/automation_writer.h"
#include "sonora/model/history.h"
#include "sonora/playback_plan.h"

namespace {

using namespace model_test;
using sonora::automation_pass_edits;
using sonora::DroppedLane;
using sonora::kAutomationWriteLabel;
using sonora::live_pass_write;
using sonora::PassWrite;
using sonora::PassWrites;
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

// The lane as played: linear between points, held at the ends, a step's
// second point from its frame.
float value_at(const std::vector<AutomationPoint>& p, Frames t) {
    if (t < p.front().at) return p.front().value;
    if (t >= p.back().at) return p.back().value;
    std::size_t i = 1;
    while (p[i].at <= t) ++i;
    const AutomationPoint& a = p[i - 1];
    const AutomationPoint& b = p[i];
    return a.value + static_cast<float>(static_cast<double>(t - a.at) /
                                        static_cast<double>(b.at - a.at) * (b.value - a.value));
}

// PR #178 review B1: a Write pass sends one write per parameter per block,
// held or not; an hour of it must not hold one entry per block.
int test_a_held_pass_stays_small() {
    PassWrites w;
    constexpr Frames kBlock = 64;
    for (Frames f = 0; f < 200000 * kBlock; f += kBlock) {
        for (Id t = 1; t <= 8; ++t) {
            w.add({t, AutomationParam::Volume, f, kBlock, -6.0f});
            w.add({t, AutomationParam::Pan, f, kBlock, 0.25f});
        }
    }
    CHECK(w.writes().size() <= 2 * 16);
    w.clear();
    CHECK(w.writes().empty());
    return 0;
}

int test_a_moving_ride_stays_small_and_close() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    constexpr Frames kBlock = 64;
    constexpr int kBlocks = 100000;
    const auto ride_at = [](Frames f) {
        return -10.0f + 6.0f * static_cast<float>(std::sin(f * 2.0 * 3.141592653589793 / 192000.0));
    };
    PassWrites w;
    for (int b = 0; b < kBlocks; ++b) {
        const Frames f = b * kBlock;
        w.add({vox.track, AutomationParam::Volume, f, kBlock, ride_at(f)});
    }
    CHECK(w.writes().size() < kBlocks / 20);
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w.writes(), 0);
    CHECK(edits.size() == 1);
    const std::vector<AutomationPoint>& p = edits[0].points;
    float worst = 0.0f;
    for (int b = 0; b < kBlocks; ++b) {
        const Frames f = b * kBlock;
        worst = std::max(worst, std::fabs(value_at(p, f) - ride_at(f)));
    }
    CHECK(worst <= 0.15f + 1e-3f);
    return 0;
}

// A loop's second turn over part of a merged first turn still wins, and the
// first turn's remainder keeps its slope.
int test_a_later_turn_over_a_merged_run() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    PassWrites w;
    for (Frames f = 0; f < 64000; f += 64) {
        w.add({vox.track, AutomationParam::Volume, f, 64, -20.0f + 20.0f * f / 64000});
    }
    for (Frames f = 32000; f < 48000; f += 64) {
        w.add({vox.track, AutomationParam::Volume, f, 64, 3.0f});
    }
    CHECK(w.writes().size() <= 6);
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w.writes(), 0);
    CHECK(edits.size() == 1);
    const std::vector<AutomationPoint>& p = edits[0].points;
    CHECK(std::fabs(value_at(p, 16000) - -15.0f) < 0.15f);
    CHECK(std::fabs(value_at(p, 31999) - -10.0f) < 0.15f);
    CHECK(value_at(p, 40000) == 3.0f);
    CHECK(std::fabs(value_at(p, 56000) - -2.5f) < 0.15f);
    // A release ends the run; the next write starts a new one.
    w.add({vox.track, AutomationParam::Volume, 64000, 0, 0.0f});
    w.add({vox.track, AutomationParam::Volume, 64000, 64, 0.0f});
    CHECK(w.writes().back().frame == 64000 && w.writes().back().frames == 64);
    CHECK(w.writes()[w.writes().size() - 2].frames == 0);
    return 0;
}

// #178 review: one lane past kMaxAutomationPoints after thinning failed the
// whole Compound, losing every lane of the pass. Only that lane's edit is
// dropped now, and named. Its 70,000 writes also outgrow the 16,384 the
// server reserves: a long pass keeps every write (long-pass growth).
int test_an_overfull_lane_is_dropped_alone() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddTrack gtr = add_track(ids, "Gtr");
    CHECK_APPLY(h, gtr);
    PassWrites w;
    w.reserve(16384);
    constexpr int kWrites = 70000;
    for (int i = 0; i < kWrites; ++i) {
        // A 12 dB zigzag: no tolerance the writer tries flattens it.
        const Frames f = static_cast<Frames>(i) * 64;
        w.add({vox.track, AutomationParam::Volume, f, 64, i % 2 == 0 ? -6.0f : 6.0f});
        w.add({gtr.track, AutomationParam::Pan, f, 64, -0.5f});
    }
    std::size_t vox_writes = 0;
    for (const PassWrite& p : w.writes()) vox_writes += p.track == vox.track ? 1 : 0;
    CHECK(vox_writes == static_cast<std::size_t>(kWrites));
    std::vector<DroppedLane> dropped;
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w.writes(), 0, &dropped);
    CHECK(edits.size() == 1);
    if (edits.size() != 1) return 1;
    CHECK(edits[0].track == gtr.track && edits[0].param == AutomationParam::Pan);
    CHECK(dropped.size() == 1);
    if (dropped.size() != 1) return 1;
    CHECK(dropped[0].track == vox.track && dropped[0].param == AutomationParam::Volume);
    CHECK(dropped[0].points > kMaxAutomationPoints);
    Compound pass;
    pass.label = kAutomationWriteLabel;
    for (const EditAutomation& e : edits) pass.ops.emplace_back(e);
    CHECK_APPLY(h, pass);
    CHECK(h.current().edit->tracks[1]->lane(AutomationParam::Pan) != nullptr);
    CHECK(h.current().edit->tracks[0]->lane(AutomationParam::Volume) == nullptr);
    return 0;
}

// A lane whose old points leave too little room for the pass at the lane's
// tolerance is thinned again at a wider one, and lands whole.
int test_a_crowded_lane_is_thinned_harder() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    constexpr std::size_t kRoom = 50;
    std::vector<AutomationPoint> old;
    for (std::size_t i = 0; i < kMaxAutomationPoints - kRoom; ++i) {
        old.push_back({100000 + static_cast<Frames>(i) * 10, i % 2 == 0 ? -20.0f : -19.0f});
    }
    CHECK_APPLY(h, (EditAutomation{vox.track, AutomationParam::Volume, 0, kMaxInteger, old}));
    // A 0.3 dB wobble: kept point by point at 0.1 and 0.2 dB, not at 0.4.
    std::vector<PassWrite> w;
    for (int i = 0; i < 1000; ++i) {
        w.push_back({vox.track, AutomationParam::Volume, static_cast<Frames>(i) * 64, 64,
                     i % 2 == 0 ? -6.0f : -5.7f});
    }
    std::vector<DroppedLane> dropped;
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w, 0, &dropped);
    CHECK(dropped.empty());
    CHECK(edits.size() == 1);
    if (edits.size() != 1) return 1;
    CHECK(edits[0].points.size() <= kRoom);
    CHECK_APPLY(h, edits[0]);
    const AutomationLane* lane = h.current().edit->tracks[0]->lane(AutomationParam::Volume);
    CHECK(lane != nullptr && lane->points.size() <= kMaxAutomationPoints);
    if (lane != nullptr) CHECK(std::fabs(value_at(lane->points, 32000) - -5.85f) <= 0.6f);
    return 0;
}

// #178 re-review: a merged line's end, value + (to - value) in float, can
// miss `to` by an ulp, and the next span then starts with a same-frame pair
// that thinning keeps as a step. An uncut line ends on `to` exactly.
int test_a_merged_line_ends_on_its_value() {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    float from = 0.0f;
    float to = 0.0f;
    const float tries[] = {-20.0f, -13.7f, -0.3f, 0.7f, 1.1f, 3.3f, 0.1f, -7.9f, 11.9f};
    for (const float a : tries) {
        for (const float b : tries) {
            volatile float d = b - a;
            volatile float end = a + d;
            if (end != b && from == to) {
                from = a;
                to = b;
            }
        }
    }
    CHECK(from != to);
    std::vector<PassWrite> w;
    PassWrite line{vox.track, AutomationParam::Volume, 0, 1000, from};
    line.to = to;
    line.line = true;
    w.push_back(line);
    w.push_back({vox.track, AutomationParam::Volume, 1000, 1000, to});
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w, 0);
    CHECK(edits.size() == 1);
    if (edits.size() != 1) return 1;
    std::size_t at_join = 0;
    for (const AutomationPoint& p : edits[0].points) at_join += p.at == 1000 ? 1 : 0;
    CHECK(at_join == 1);
    CHECK(has(edits[0].points, 1000, to));
    return 0;
}

// #178 re-review: std::clamp passes NaN through, so a NaN pan reached the
// lane. A non-finite live value is never written, on either parameter.
int test_a_non_finite_live_value_is_dropped() {
    sonora::AutomationWrite a{};
    a.pass = 1;
    a.frame = 640;
    a.frames = 64;
    a.param = sonora::kAutoPan;
    PassWrite p;
    a.value = std::nanf("");
    CHECK(!live_pass_write(a, 7, p));
    a.value = std::numeric_limits<float>::infinity();
    CHECK(!live_pass_write(a, 7, p));
    a.param = sonora::kAutoVolume;
    a.value = std::nanf("");
    CHECK(!live_pass_write(a, 7, p));
    a.value = std::numeric_limits<float>::infinity();
    CHECK(!live_pass_write(a, 7, p));
    a.param = sonora::kAutoParams;
    a.value = 0.5f;
    CHECK(!live_pass_write(a, 7, p));

    // What is finite is written in the lane's unit, clamped to its range.
    a.param = sonora::kAutoPan;
    a.value = 1.5f;
    CHECK(live_pass_write(a, 7, p));
    CHECK(p.track == 7 && p.param == AutomationParam::Pan && p.value == 1.0f);
    CHECK(p.frame == 640 && p.frames == 64 && !p.line);
    a.param = sonora::kAutoVolume;
    a.value = 0.5f;
    CHECK(live_pass_write(a, 7, p));
    CHECK(p.param == AutomationParam::Volume && std::fabs(p.value - -6.0206f) < 1e-3f);
    a.value = 0.0f;
    CHECK(live_pass_write(a, 7, p) && p.value == kMinGainDb);
    return 0;
}

// The trim fader (ADR-0027 §8 slice 5): written to the VolumeTrim lane in
// dB, clamped to [-60, +12]; a track with no trim lane plays 0 dB of trim,
// which is what a run steps in from and out to.
int test_a_trim_write_is_its_own_lane() {
    sonora::AutomationWrite a{};
    a.frame = 640;
    a.frames = 64;
    a.param = sonora::kAutoTrim;
    a.value = 0.5f;
    PassWrite p;
    CHECK(live_pass_write(a, 7, p));
    CHECK(p.param == AutomationParam::VolumeTrim && std::fabs(p.value - -6.0206f) < 1e-3f);
    a.value = 4.0f;
    CHECK(live_pass_write(a, 7, p) && p.value == kMaxTrimDb);
    a.value = 0.0f;
    CHECK(live_pass_write(a, 7, p) && p.value == kMinTrimDb);
    a.value = std::nanf("");
    CHECK(!live_pass_write(a, 7, p));

    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    TrackMix m = *h.current().mix.find(vox.track);
    m.gain_db = -3.0f;
    CHECK_APPLY(h, SetMix{m});
    std::vector<PassWrite> w;
    ride(w, vox.track, AutomationParam::VolumeTrim, 200, 400, 6.0f);
    const std::vector<EditAutomation> edits = automation_pass_edits(h.current(), w, 0);
    CHECK(edits.size() == 1 && edits[0].param == AutomationParam::VolumeTrim);
    CHECK(has(edits[0].points, 200, 0.0f) && has(edits[0].points, 200, 6.0f));
    CHECK(has(edits[0].points, 400, 6.0f) && has(edits[0].points, 400, 0.0f));
    CHECK_APPLY(h, edits[0]);
    CHECK(h.current().edit->tracks[0]->lane(AutomationParam::Volume) == nullptr);
    CHECK(h.current().edit->tracks[0]->lane(AutomationParam::VolumeTrim) != nullptr);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_thin();
    failed += test_a_run_over_a_lane();
    failed += test_no_lane_keeps_the_static_fader();
    failed += test_release_returns_and_later_turns_win();
    failed += test_a_held_pass_stays_small();
    failed += test_a_moving_ride_stays_small_and_close();
    failed += test_a_later_turn_over_a_merged_run();
    failed += test_an_overfull_lane_is_dropped_alone();
    failed += test_a_crowded_lane_is_thinned_harder();
    failed += test_a_merged_line_ends_on_its_value();
    failed += test_a_non_finite_live_value_is_dropped();
    failed += test_a_trim_write_is_its_own_lane();
    if (failed != 0) {
        std::fprintf(stderr, "automation_writer: %d failed\n", failed);
        return 1;
    }
    std::printf("automation_writer: ok\n");
    return 0;
}
