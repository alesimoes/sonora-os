// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation lanes in the Edit window (ADR-0027, BACKLOG `automation.lanes`),
// without a window: the operations a gesture is built into, the controller's
// mouse and Edit-command paths, the rows lanes make taller, and what the
// timeline draws. Every edit must be one EditAutomation, so one undo step.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "automation_edit.h"
#include "edit_controller.h"
#include "peak_source.h"
#include "theme.h"
#include "timeline_document.h"
#include "timeline_scene.h"

using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                                \
        }                                                                            \
    } while (0)

namespace {

int checks = 0;
#define OK(cond)     \
    do {             \
        CHECK(cond); \
        ++checks;    \
    } while (0)

constexpr uint32_t kRate = 48000;

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

AutomationLane lane_of(AutomationParam p, std::vector<AutomationPoint> pts) {
    AutomationLane l;
    l.param = p;
    l.points = std::move(pts);
    return l;
}

const AutomationLane* lane(const TimelineDocument& doc, int track, AutomationParam p) {
    return doc.snapshot().edit->tracks[static_cast<std::size_t>(track)]->lane(p);
}

bool valid(const Snapshot& s) {
    std::string error;
    const bool ok = check_invariants(s, error);
    if (!ok) std::fprintf(stderr, "  invariant: %s\n", error.c_str());
    return ok;
}

// A volume lane set up through the same operation the gestures send.
bool seed(ModelDocument& doc, int track, AutomationParam p, std::vector<AutomationPoint> pts) {
    EditAutomation op;
    op.track = doc.snapshot().edit->tracks[static_cast<std::size_t>(track)]->id;
    op.param = p;
    op.from = 0;
    op.to = kMaxInteger;
    op.points = std::move(pts);
    std::string why;
    return doc.submit(op, why);
}

int test_values() {
    const AutomationLane l = lane_of(AutomationParam::Volume, {{1000, -20.0f}, {2000, 0.0f}, {2000, -6.0f}, {3000, -6.0f}});
    OK(lane_value_at(nullptr, 5, 1.5f) == 1.5f);
    OK(lane_value_at(&l, 0, 9.0f) == -20.0f);          // held before the first point
    OK(near(lane_value_at(&l, 1500, 9.0f), -10.0f));   // linear between
    OK(lane_value_at(&l, 2000, 9.0f) == -6.0f);        // a step: the value after it
    OK(lane_value_at(&l, 9000, 9.0f) == -6.0f);        // held after the last
    // The taper puts unity above the middle of the lane, and round-trips.
    OK(lane_position(AutomationParam::Volume, 0.0f) > 0.7f);
    OK(near(lane_value(AutomationParam::Volume, lane_position(AutomationParam::Volume, -12.0f)), -12.0f, 0.1f));
    OK(lane_position(AutomationParam::Pan, 0.0f) == 0.5f);
    OK(lane_position(AutomationParam::Pan, -1.0f) == 0.0f && lane_position(AutomationParam::Pan, 1.0f) == 1.0f);
    OK(clamp_lane_value(AutomationParam::Pan, 3.0f) == 1.0f);
    OK(clamp_lane_value(AutomationParam::Volume, 99.0f) == kMaxGainDb);
    OK(lane_value_text(AutomationParam::Volume, -6.0f) == "-6.0 dB");
    return 0;
}

// A plugin lane is not clamped, so two finite points can differ by more than
// a float holds. Interpolating in float made that segment infinite: drawn at
// infinity, and never thinned.
int test_wide_plugin_lane() {
    const float big = 3.0e38f;
    const AutomationLane l = lane_of(AutomationParam::Plugin, {{0, -big}, {2000, big}});
    for (Frames f : {Frames{1}, Frames{500}, Frames{1500}}) {
        OK(std::isfinite(lane_value_at(&l, f, 0.0f)));
    }
    OK(std::fabs(lane_value_at(&l, 500, 0.0f) + big / 2) <= big * 1e-6f);
    OK(std::fabs(lane_value_at(&l, 1000, 0.0f)) <= big * 1e-6f);
    // Collinear: the middle point adds nothing to the curve.
    const std::vector<AutomationPoint> three{{0, -big}, {1000, 0.0f}, {2000, big}};
    OK(thin_points(three, thin_tolerance(AutomationParam::Plugin)) == l.points);
    return 0;
}

int test_operations() {
    ModelDocument doc(make_empty_session(2, kRate));
    std::string why;
    const Id t0 = doc.snapshot().edit->tracks[0]->id;

    // A breakpoint on a track with no lane makes the lane.
    OK(lane(doc, 0, AutomationParam::Volume) == nullptr);
    OK(doc.submit(add_point_op(t0, AutomationParam::Volume, 4800, -3.0f), why));
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 1);
    OK(valid(doc.snapshot()));
    OK(doc.submit(add_point_op(t0, AutomationParam::Volume, 9600, -12.0f), why));
    OK(doc.submit(add_point_op(t0, AutomationParam::Volume, 19200, 0.0f), why));
    const AutomationLane* l = lane(doc, 0, AutomationParam::Volume);
    OK(l->points.size() == 3 && l->points[1].at == 9600);
    // A point on a frame that has one replaces it.
    OK(doc.submit(add_point_op(t0, AutomationParam::Volume, 9600, -9.0f), why));
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 3);
    OK(lane(doc, 0, AutomationParam::Volume)->points[1].value == -9.0f);

    // A move stops against its neighbours rather than reordering the lane.
    EditAutomation op;
    l = lane(doc, 0, AutomationParam::Volume);
    OK(move_point_op(t0, *l, 1, 99999999, -1.0f, op));
    OK(op.from == 9600 && op.to == 19200);   // [old, new + 1): new is 19199
    OK(op.points.size() == 1 && op.points[0].at == 19199);
    OK(doc.submit(op, why));
    OK(lane(doc, 0, AutomationParam::Volume)->points[1].at == 19199);
    OK(valid(doc.snapshot()));
    OK(!move_point_op(t0, *l, 7, 0, 0.0f, op));   // not a point

    // Deleting removes the point; deleting the last one removes the lane.
    l = lane(doc, 0, AutomationParam::Volume);
    OK(delete_point_op(t0, *l, 0, op) && op.points.empty());
    OK(doc.submit(op, why));
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 2);
    for (int i = 0; i < 2; ++i) {
        OK(delete_point_op(t0, *lane(doc, 0, AutomationParam::Volume), 0, op));
        OK(doc.submit(op, why));
    }
    OK(lane(doc, 0, AutomationParam::Volume) == nullptr);
    OK(valid(doc.snapshot()));

    // One of a step's two points keeps its frame when moved, and deleting
    // one leaves the other.
    OK(seed(doc, 0, AutomationParam::Pan, {{1000, -1.0f}, {5000, -1.0f}, {5000, 1.0f}, {9000, 1.0f}}));
    l = lane(doc, 0, AutomationParam::Pan);
    OK(move_point_op(t0, *l, 2, 7000, 0.5f, op));
    OK(op.points.size() == 2 && op.points[0].at == 5000 && op.points[1].at == 5000 &&
       op.points[1].value == 0.5f);
    OK(doc.submit(op, why));
    OK(valid(doc.snapshot()));
    OK(delete_point_op(t0, *lane(doc, 0, AutomationParam::Pan), 1, op));
    OK(op.points.size() == 1 && op.points[0].value == 0.5f);

    // Clear keeps the shape outside the range and runs straight across it.
    OK(seed(doc, 1, AutomationParam::Volume, {{0, -30.0f}, {10000, -10.0f}, {20000, -10.0f}, {30000, 0.0f}}));
    const Id t1 = doc.snapshot().edit->tracks[1]->id;
    const AutomationLane before = *lane(doc, 1, AutomationParam::Volume);
    l = lane(doc, 1, AutomationParam::Volume);
    OK(clear_range_op(t1, *l, 5000, 25000, op));
    OK(doc.submit(op, why));
    const AutomationLane* after = lane(doc, 1, AutomationParam::Volume);
    OK(near(lane_value_at(after, 4000, 0), lane_value_at(&before, 4000, 0)));
    OK(near(lane_value_at(after, 26000, 0), lane_value_at(&before, 26000, 0)));
    OK(near(lane_value_at(after, 5000, 0), lane_value_at(&before, 5000, 0)));
    bool inside = false;
    for (const AutomationPoint& p : after->points) inside = inside || (p.at > 5000 && p.at < 24999);
    OK(!inside);
    // No breakpoint in a range: nothing to clear.
    OK(!clear_range_op(t1, *after, 6000, 7000, op));
    // Clearing the whole lane removes it.
    OK(clear_range_op(t1, *after, 0, 40000, op));
    OK(op.points.empty());
    OK(doc.submit(op, why));
    OK(lane(doc, 1, AutomationParam::Volume) == nullptr);
    OK(valid(doc.snapshot()));
    return 0;
}

int test_stroke() {
    // A drawn ramp of one sample a pixel is a line: two breakpoints.
    std::vector<AutomationPoint> ramp;
    for (int i = 0; i < 200; ++i) ramp.push_back({i * 1200, -40.0f + i * 0.1f});
    EditAutomation op;
    OK(stroke_op(7, AutomationParam::Volume, ramp, op));
    OK(op.points.size() == 2 && op.from == 0 && op.to == 199 * 1200 + 1);
    // A corner survives the thinning.
    std::vector<AutomationPoint> corner;
    for (int i = 0; i < 50; ++i) corner.push_back({i * 1200, -40.0f + i * 0.8f});
    for (int i = 50; i < 100; ++i) corner.push_back({i * 1200, 0.0f - (i - 50) * 0.8f});
    OK(stroke_op(7, AutomationParam::Volume, corner, op));
    OK(op.points.size() == 3 && op.points[1].at == 50 * 1200);
    // A stroke drawn backwards, or over itself, still lands sorted, the
    // latest sample at a frame winning.
    OK(stroke_op(7, AutomationParam::Pan, {{3000, 0.5f}, {1000, -0.5f}, {3000, 0.25f}}, op));
    OK(op.points.size() == 2 && op.points[0].at == 1000 && op.points[1].value == 0.25f);
    ModelDocument doc(make_empty_session(1, kRate));
    std::string why;
    op.track = doc.snapshot().edit->tracks[0]->id;
    OK(doc.submit(op, why));
    OK(valid(doc.snapshot()));
    OK(!stroke_op(7, AutomationParam::Pan, {}, op));
    return 0;
}

int test_clip() {
    ModelDocument doc(make_empty_session(2, kRate));
    std::string why;
    OK(seed(doc, 0, AutomationParam::Volume, {{0, -20.0f}, {10000, -10.0f}, {20000, 0.0f}, {30000, -30.0f}}));
    const Id t1 = doc.snapshot().edit->tracks[1]->id;
    AutomationClip clip;
    OK(!copy_lane_range(nullptr, 0, 100, clip));
    OK(copy_lane_range(lane(doc, 0, AutomationParam::Volume), 5000, 25000, clip));
    OK(clip.length == 20000 && clip.points.front().at == 0 && clip.points.back().at == 19999);
    OK(near(clip.points.front().value, -15.0f) && near(clip.points.back().value, -15.0f, 0.1f));
    OK(clip.points.size() == 4);   // both edges and the two points inside
    EditAutomation op;
    OK(paste_op(t1, clip, 50000, op));
    OK(op.from == 50000 && op.to == 70000 && op.points.front().at == 50000);
    OK(doc.submit(op, why));
    const AutomationLane* pasted = lane(doc, 1, AutomationParam::Volume);
    OK(pasted != nullptr);
    OK(near(lane_value_at(pasted, 60000, 0), lane_value_at(lane(doc, 0, AutomationParam::Volume), 15000, 0), 0.1f));
    OK(valid(doc.snapshot()));
    OK(!paste_op(t1, AutomationClip{}, 0, op));
    return 0;
}

// --- the controller --------------------------------------------------------------

int test_rows() {
    ModelDocument doc(make_empty_session(3, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    const double h = ctl.view().track_height;
    // Without lanes the rows are what they were.
    OK(ctl.view().tops.empty() && ctl.view().y_of_track(2) == 2 * h);
    OK(ctl.view().track_at(2 * h + 1) == 2);
    OK(ctl.view().content_height(3) == 3 * h);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    const ViewState& v = ctl.view();
    OK(ctl.autolanes_shown(0) && !ctl.autolanes_shown(1));
    OK(v.tops.size() == 4 && v.tops[1] == h + kLaneCount * v.autolane_height);
    OK(v.y_of_track(1) == h + kLaneCount * v.autolane_height);
    OK(v.content_height(3) == 3 * h + kLaneCount * v.autolane_height);
    // A pixel in a lane is over its track, and says which lane.
    const double in_lane1 = v.y_of_autolane(0, 1) + 3;
    OK(v.track_at(in_lane1) == 0 && v.autolane_at(0, in_lane1) == 1);
    OK(v.autolane_at(0, h / 2) == -1);
    OK(v.track_at(v.y_of_track(1) + 1) == 1);
    const Hit hit = ctl.hit(50, in_lane1);
    OK(hit.track == 0 && hit.autolane == 1 && hit.zone == HitZone::Automation);
    // The lane's position maps back to the pixel it was drawn at.
    OK(std::fabs(v.autolane_position(0, 0, v.autolane_y(0, 0, 0.3f)) - 0.3f) < 1e-4);
    // Hidden again, and reset when a session opens.
    OK(ctl.toggle_autolanes(0, why));
    OK(!ctl.autolanes_shown(0) && ctl.view().tops.empty());
    OK(ctl.toggle_autolanes(2, why));
    ctl.document_replaced();
    OK(!ctl.autolanes_shown(2) && ctl.view().tops.empty());
    OK(!ctl.toggle_autolanes(9, why));
    return 0;
}

int test_lane_height_follows_zoom() {
    ModelDocument doc(make_empty_session(3, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    const double base = ctl.view().autolane_height;
    ctl.vertical_zoom(true);
    OK(ctl.view().autolane_height > base);
    OK(std::fabs(ctl.view().autolane_height - base * 1.5) < 1e-6);
    // The rows below follow it.
    OK(std::fabs(ctl.view().y_of_track(1) - (ctl.view().track_height + kLaneCount * base * 1.5)) <
       1e-6);
    ctl.vertical_zoom(false);
    ctl.vertical_zoom(false);
    OK(ctl.view().autolane_height < base);
    OK(ctl.view().autolane_height >= 32.0);
    for (int i = 0; i < 20; ++i) ctl.vertical_zoom(true);
    OK(ctl.view().autolane_height <= 224.0);
    return 0;
}

int test_gestures() {
    ModelDocument doc(make_empty_session(2, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    const ViewState& v = ctl.view();
    const double fpp = v.fpp;
    const auto xf = [&](Frames f) { return static_cast<double>(f) / fpp; };
    const double unity_y = v.autolane_y(0, 0, lane_position(AutomationParam::Volume, 0.0f));
    const double low_y = v.autolane_y(0, 0, lane_position(AutomationParam::Volume, -30.0f));

    // A click with the Smart Tool adds one breakpoint: one undo step.
    const uint64_t r0 = doc.snapshot().revision;
    OK(ctl.press(xf(120000), unity_y, why));
    OK(ctl.drag().kind == Drag::Kind::Point && ctl.drag().point == -1);
    OK(doc.snapshot().revision == r0);   // nothing is sent before the release
    OK(ctl.release(xf(120000), unity_y, why));
    OK(doc.snapshot().revision == r0 + 1);
    const AutomationLane* l = lane(doc, 0, AutomationParam::Volume);
    OK(l != nullptr && l->points.size() == 1 && l->points[0].at == 120000);
    OK(near(l->points[0].value, 0.0f, 0.5f));
    OK(ctl.selection().autolane == 0 && ctl.selection().point == 0);
    OK(doc.undo_label() == "EditAutomation");
    OK(doc.undo(why));
    OK(lane(doc, 0, AutomationParam::Volume) == nullptr);
    OK(doc.redo(why));

    // A second, then a drag of the first past the second stops against it.
    OK(ctl.press(xf(240000), low_y, why) && ctl.release(xf(240000), low_y, why));
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 2);
    const uint64_t r1 = doc.snapshot().revision;
    OK(ctl.press(xf(120000), unity_y, why));
    OK(ctl.drag().point == 0);
    OK(ctl.selection().point == 0);
    ctl.move(xf(300000), low_y);
    OK(ctl.drag().moved);
    OK(ctl.drag().at_point.at == 239999);
    OK(doc.snapshot().revision == r1);
    OK(ctl.release(xf(300000), low_y, why));
    OK(doc.snapshot().revision == r1 + 1);
    l = lane(doc, 0, AutomationParam::Volume);
    OK(l->points.size() == 2 && l->points[0].at == 239999 && near(l->points[0].value, -30.0f, 2.0f));
    OK(valid(doc.snapshot()));

    // A click on a point selects it and sends nothing; Clear deletes it.
    const double x0 = xf(l->points[0].at);
    const double y0 = v.autolane_y(0, 0, lane_position(AutomationParam::Volume, l->points[0].value));
    const uint64_t r2 = doc.snapshot().revision;
    OK(ctl.press(x0, y0, why) && ctl.release(x0, y0, why));
    OK(doc.snapshot().revision == r2 && ctl.selection().point == 0);
    OK(ctl.clear(why));
    OK(doc.snapshot().revision == r2 + 1);
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 1);
    OK(ctl.selection().point == -1);

    // The Pencil draws a stroke as one edit, thinned.
    ctl.set_tool(Tool::Pencil);
    const double py = v.autolane_y(1 == 1 ? 0 : 0, 1, 0.5f);
    const uint64_t r3 = doc.snapshot().revision;
    OK(ctl.press(100, py, why));
    OK(ctl.drag().kind == Drag::Kind::Stroke);
    for (int x = 101; x <= 400; ++x) {
        ctl.move(x, v.autolane_y(0, 1, 0.5f + (x - 100) * 0.001f));
    }
    OK(ctl.drag().stroke.size() > 100);
    OK(doc.snapshot().revision == r3);
    OK(ctl.release(400, v.autolane_y(0, 1, 0.8f), why));
    OK(doc.snapshot().revision == r3 + 1);
    const AutomationLane* pan = lane(doc, 0, AutomationParam::Pan);
    OK(pan != nullptr && pan->points.size() <= 4 && pan->points.size() >= 2);
    OK(valid(doc.snapshot()));
    // The Pencil on a track's own row does nothing.
    const uint64_t r4 = doc.snapshot().revision;
    OK(ctl.press(100, v.y_of_track(0) + 20, why) && ctl.release(300, v.y_of_track(0) + 20, why));
    OK(doc.snapshot().revision == r4);

    // Trim does nothing in a lane.
    ctl.set_tool(Tool::Trim);
    OK(ctl.press(100, unity_y, why) && ctl.release(100, unity_y, why));
    OK(doc.snapshot().revision == r4);
    return 0;
}

int test_range_commands() {
    ModelDocument doc(make_empty_session(2, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    OK(ctl.toggle_autolanes(1, why));
    const ViewState& v = ctl.view();
    const auto xf = [&](Frames f) { return static_cast<double>(f) / v.fpp; };
    OK(seed(doc, 0, AutomationParam::Volume, {{0, -20.0f}, {120000, -10.0f}, {240000, 0.0f}, {360000, -30.0f}}));
    OK(valid(doc.snapshot()));

    // The Selector selects a range of the lane; Copy then holds it and
    // changes nothing in the document.
    ctl.set_tool(Tool::Select);
    const double y = v.autolane_y(0, 0, 0.5f);
    const uint64_t r0 = doc.snapshot().revision;
    OK(ctl.press(xf(60000), y, why));
    // Past the last track the range still reaches it, on the same lane: the
    // other lane is not reachable.
    ctl.move(xf(300000), y + 2000);
    OK(ctl.release(xf(300000), y + 2000, why));
    OK(ctl.selection().autolane == 0 && ctl.selection().track == 0 && ctl.selection().tracks == 2);
    OK(ctl.selection().start == 60000 && ctl.selection().end == 300000);
    OK(ctl.copy(why));
    OK(doc.snapshot().revision == r0 && ctl.automation_clipboard().size() == 2);
    OK(!ctl.automation_clipboard()[0].empty() && ctl.automation_clipboard()[1].empty());
    OK(ctl.automation_clipboard().front().param == AutomationParam::Volume);

    // Paste on another track's lane is one edit.
    OK(ctl.press(xf(30000), v.autolane_y(1, 0, 0.5f), why));
    OK(ctl.release(xf(30000), v.autolane_y(1, 0, 0.5f), why));
    OK(ctl.selection().track == 1 && ctl.selection().autolane == 0);
    OK(ctl.paste(why));
    OK(doc.snapshot().revision == r0 + 1);
    const AutomationLane* pasted = lane(doc, 1, AutomationParam::Volume);
    OK(pasted != nullptr && pasted->points.front().at == 30000);
    OK(ctl.selection().start == 30000 && ctl.selection().end == 270000);
    OK(valid(doc.snapshot()));

    // A pan clipboard is not pasted into a volume lane, and says so.
    OK(seed(doc, 0, AutomationParam::Pan, {{0, -0.5f}, {100000, 0.5f}}));
    OK(ctl.press(xf(0), v.autolane_y(0, 1, 0.5f), why));
    ctl.move(xf(90000), v.autolane_y(0, 1, 0.5f));
    OK(ctl.release(xf(90000), v.autolane_y(0, 1, 0.5f), why));
    OK(ctl.selection().autolane == 1 && ctl.copy(why));
    OK(ctl.automation_clipboard().front().param == AutomationParam::Pan);
    OK(ctl.press(xf(10000), v.autolane_y(1, 0, 0.5f), why) && ctl.release(xf(10000), v.autolane_y(1, 0, 0.5f), why));
    const uint64_t r1 = doc.snapshot().revision;
    OK(!ctl.paste(why) && why.find("Pan automation") != std::string::npos);
    OK(doc.snapshot().revision == r1);

    // Cut removes the range's breakpoints and holds them; Clear needs a
    // range and a breakpoint in it.
    OK(ctl.press(xf(60000), y, why));
    ctl.move(xf(300000), y);
    OK(ctl.release(xf(300000), y, why));
    OK(ctl.cut(why));
    OK(doc.snapshot().revision == r1 + 1);
    const AutomationLane* after = lane(doc, 0, AutomationParam::Volume);
    for (const AutomationPoint& p : after->points) OK(p.at <= 60000 || p.at >= 299999);
    OK(ctl.automation_clipboard().front().param == AutomationParam::Volume);
    OK(!ctl.clear(why) && why.find("no breakpoint") != std::string::npos);
    OK(doc.snapshot().revision == r1 + 1);
    // A track's own row goes back to the regions: no range, no automation.
    OK(ctl.press(xf(60000), v.y_of_track(0) + 10, why) && ctl.release(xf(60000), v.y_of_track(0) + 10, why));
    OK(ctl.selection().autolane == -1);
    OK(!ctl.clear(why) && why.find("nothing is selected") != std::string::npos);

    // Thin: a dense ramp loses its middle in one undo step; a lane already
    // thin is refused without an edit.
    std::vector<AutomationPoint> ramp;
    for (int i = 0; i <= 10; ++i) ramp.push_back({i * 10000, -40.0f + 2.0f * i});
    OK(seed(doc, 0, AutomationParam::Volume, ramp));
    OK(ctl.press(xf(0), y, why));
    ctl.move(xf(100000), y);
    OK(ctl.release(xf(100000), y, why));
    const uint64_t r2 = doc.snapshot().revision;
    OK(ctl.thin_automation(why));
    OK(doc.snapshot().revision == r2 + 1);
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() < 11);
    OK(valid(doc.snapshot()));
    OK(!ctl.thin_automation(why) && why.find("nothing to thin") != std::string::npos);
    OK(doc.snapshot().revision == r2 + 1);
    OK(ctl.press(xf(60000), v.y_of_track(0) + 10, why) && ctl.release(xf(60000), v.y_of_track(0) + 10, why));
    OK(!ctl.thin_automation(why) && why.find("select a range") != std::string::npos);
    return 0;
}

// Automation Follows Edit: what a region move or clear adds for each lane.
int test_follow_ops() {
    TrackEdit t;
    t.id = 7;
    OK(follow_move_ops(t, 1000, 2000, 5000).empty());   // no lane
    OK(follow_clear_ops(t, 1000, 2000).empty());

    // A flat lane plays the same whatever moves over it.
    t.automation = {std::make_shared<AutomationLane>(
        lane_of(AutomationParam::Volume, {{0, -6.0f}, {9000, -6.0f}}))};
    OK(follow_move_ops(t, 1000, 2000, 5000).empty());
    OK(follow_clear_ops(t, 1000, 2000).empty());

    // A dip on [1000, 2000) moved to 5000: one EditAutomation a lane, over
    // both spans, its ends included.
    const AutomationLane dip = lane_of(
        AutomationParam::Volume, {{0, 0.0f}, {1200, 0.0f}, {1500, -30.0f}, {1800, 0.0f}, {9000, -9.0f}});
    t.automation = {std::make_shared<AutomationLane>(dip),
                    std::make_shared<AutomationLane>(
                        lane_of(AutomationParam::Pan, {{0, -1.0f}, {9000, 1.0f}}))};
    OK(follow_move_ops(t, 1000, 2000, 1000).empty());   // nowhere to go
    const std::vector<EditAutomation> ops = follow_move_ops(t, 1000, 2000, 5000);
    OK(ops.size() == 2);
    OK(ops[0].track == 7 && ops[0].param == AutomationParam::Volume);
    OK(ops[0].from == 1000 && ops[0].to == 6001);
    OK(ops[1].param == AutomationParam::Pan);
    // Spliced as apply() does.
    AutomationLane moved = dip;
    moved.points.clear();
    for (const AutomationPoint& p : dip.points) {
        if (p.at < ops[0].from) moved.points.push_back(p);
    }
    for (const AutomationPoint& p : ops[0].points) {
        OK(p.at >= ops[0].from && p.at < ops[0].to);
        moved.points.push_back(p);
    }
    for (const AutomationPoint& p : dip.points) {
        if (p.at >= ops[0].to) moved.points.push_back(p);
    }
    const auto& mp = moved.points;
    for (std::size_t i = 1; i < mp.size(); ++i) OK(mp[i - 1].at <= mp[i].at);
    for (std::size_t i = 2; i < mp.size(); ++i) OK(mp[i - 2].at != mp[i].at);   // two a frame at most
    for (Frames f : {Frames(5000), Frames(5300), Frames(5500), Frames(5999)}) {
        OK(near(lane_value_at(&moved, f, 9.0f), lane_value_at(&dip, f - 4000, 9.0f), 0.01f));
    }
    for (Frames f : {Frames(0), Frames(999), Frames(6000), Frames(7000), Frames(9000)}) {
        OK(near(lane_value_at(&moved, f, 9.0f), lane_value_at(&dip, f, 9.0f), 0.01f));
    }
    OK(lane_value_at(&moved, 1500, 9.0f) > -5.0f);   // the dip left its old place

    // A move onto its own span (a nudge shorter than the region) too.
    const std::vector<EditAutomation> near_ops = follow_move_ops(t, 1000, 2000, 1100);
    OK(near_ops.size() == 2 && near_ops[0].from == 1000 && near_ops[0].to == 2101);

    // A clear runs straight across [1000, 2000], its edges kept. The pan
    // lane has no point there, so already runs straight: no edit for it.
    const std::vector<EditAutomation> cleared = follow_clear_ops(t, 1000, 2000);
    OK(cleared.size() == 1 && cleared[0].param == AutomationParam::Volume);
    OK(cleared[0].from == 1000 && cleared[0].to == 2001);
    OK(cleared[0].points.size() == 2 && cleared[0].points[0].at == 1000 &&
       cleared[0].points[0].value == 0.0f && cleared[0].points[1].at == 2000 &&
       near(cleared[0].points[1].value, lane_value_at(&dip, 2000, 9.0f), 0.001f));
    OK(follow_clear_ops(t, 2500, 3000).empty());

    // A plugin lane follows as any other, and its edit names the lane's
    // insert and key: without them apply() refuses the edit, and with it
    // the whole Compound (ADR-0027 §5, update of 2026-11-01).
    AutomationLane plugin = lane_of(AutomationParam::Plugin, dip.points);
    plugin.insert = 42;
    plugin.key = "threshold";
    t.automation = {std::make_shared<AutomationLane>(plugin)};
    const std::vector<EditAutomation> plugin_moved = follow_move_ops(t, 1000, 2000, 5000);
    OK(plugin_moved.size() == 1 && plugin_moved[0].param == AutomationParam::Plugin);
    OK(plugin_moved[0].insert == 42 && plugin_moved[0].key == "threshold");
    const std::vector<EditAutomation> plugin_cleared = follow_clear_ops(t, 1000, 2000);
    OK(plugin_cleared.size() == 1 && plugin_cleared[0].insert == 42 &&
       plugin_cleared[0].key == "threshold");
    EditAutomation op;
    OK(clear_range_op(7, plugin, 1000, 2000, op) && op.insert == 42 && op.key == "threshold");
    OK(delete_point_op(7, plugin, 1, op) && op.insert == 42 && op.key == "threshold");
    OK(move_point_op(7, plugin, 1, 1300, -1.0f, op) && op.insert == 42 && op.key == "threshold");
    return 0;
}

// `op` spliced into `lane` as apply() splices it.
AutomationLane spliced(const AutomationLane& lane, const EditAutomation& op) {
    AutomationLane out = lane;
    out.points.clear();
    for (const AutomationPoint& p : lane.points) {
        if (p.at < op.from) out.points.push_back(p);
    }
    for (const AutomationPoint& p : op.points) out.points.push_back(p);
    for (const AutomationPoint& p : lane.points) {
        if (p.at >= op.to) out.points.push_back(p);
    }
    return out;
}

// Automation Follows Edit on a lane at kMaxAutomationPoints: the edge
// points a move or clear adds would make apply() refuse the lane's edit and
// with it the whole Compound. Points that play nothing (inside a flat run)
// go first; a lane that still would not fit is left behind and named, and
// the region edit goes ahead without it (BACKLOG, ADR-0027 §7).
int test_follow_at_the_lane_cap() {
    // A move leaves its old span straight, so it frees the points under it;
    // only a region over a sparse part of a full lane needs more. Here every
    // 10 frames, none in [1000, 1990] or [5000, 5990], a zigzag (no point
    // redundant, the gaps sloped) but over [flat_from, flat_to], at -3.
    const auto full = [](Frames flat_from, Frames flat_to) {
        std::vector<AutomationPoint> pts;
        for (Frames f = 0; pts.size() < kMaxAutomationPoints; f += 10) {
            if ((f >= 1000 && f <= 1990) || (f >= 5000 && f <= 5990)) continue;
            const bool flat = f >= flat_from && f <= flat_to;
            pts.push_back({f, flat ? -3.0f : ((f / 10) % 2 ? -6.0f : 0.0f)});
        }
        return std::make_shared<AutomationLane>(lane_of(AutomationParam::Volume, pts));
    };
    TrackEdit t;
    t.id = 7;
    t.automation = {full(-1, -1), std::make_shared<AutomationLane>(
                                      lane_of(AutomationParam::Pan, {{0, -1.0f}, {9000, 1.0f}}))};
    std::vector<const AutomationLane*> left;
    const std::vector<EditAutomation> moved = follow_move_ops(t, 1005, 1995, 5005, &left);
    // The pan lane still follows; the full volume lane stays and is named.
    OK(moved.size() == 1 && moved[0].param == AutomationParam::Pan);
    OK(left.size() == 1 && left[0] == t.automation[0].get());
    // A clear over one point puts two in its place.
    left.clear();
    const std::vector<EditAutomation> cleared = follow_clear_ops(t, 2008, 2012, &left);
    OK(cleared.empty() && left.size() == 1 && left[0] == t.automation[0].get());

    // A flat run inside the edit's span: its inner points play nothing, so
    // they go and the lane follows, still playing as it did.
    t.automation[0] = full(3000, 3500);
    left.clear();
    const std::vector<EditAutomation> fit = follow_move_ops(t, 1005, 1995, 5005, &left);
    OK(left.empty() && fit.size() == 2 && fit[0].param == AutomationParam::Volume);
    const AutomationLane& was = *t.automation[0];
    const AutomationLane after = spliced(was, fit[0]);
    OK(after.points.size() <= kMaxAutomationPoints);
    for (Frames f : {Frames(0), Frames(995), Frames(1004), Frames(1996), Frames(2995), Frames(3000),
                     Frames(3255), Frames(3500), Frames(3505), Frames(4990), Frames(5996),
                     Frames(7015), Frames(600000)}) {
        OK(near(lane_value_at(&after, f, 9.0f), lane_value_at(&was, f, 9.0f), 1e-5f));
    }
    for (Frames f : {Frames(5005), Frames(5500), Frames(5994)}) {
        OK(near(lane_value_at(&after, f, 9.0f), lane_value_at(&was, f - 4000, 9.0f), 1e-5f));
    }
    // Outside the edit's span nothing is dropped: a flat run there stays,
    // and the lane with it.
    t.automation[0] = full(100000, 100500);
    left.clear();
    OK(follow_move_ops(t, 1005, 1995, 5005, &left).size() == 1 && left.size() == 1);
    return 0;
}

// The Selector on track `track`'s lane `lane` from frame `a`, let go over
// lane `lane_end` of track `track_end` at frame `b`.
bool drag_lane_range(EditController& ctl, int track, int lane, Frames a, int track_end,
                     int lane_end, Frames b, std::string& why) {
    const ViewState& v = ctl.view();
    const auto xf = [&](Frames f) { return static_cast<double>(f) / v.fpp; };
    ctl.set_tool(Tool::Select);
    if (!ctl.press(xf(a), v.autolane_y(track, lane, 0.5f), why)) return false;
    ctl.move(xf(b), v.autolane_y(track_end, lane_end, 0.5f));
    return ctl.release(xf(b), v.autolane_y(track_end, lane_end, 0.5f), why);
}

// automation.thin keeps exactly what the curve needs (#199 review).
int test_thin_exact() {
    ModelDocument doc(make_empty_session(1, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    const auto points = [&](AutomationParam p) { return lane(doc, 0, p)->points; };
    // A collinear 11-point ramp keeps its two ends and nothing else.
    std::vector<AutomationPoint> ramp;
    for (int i = 0; i <= 10; ++i) ramp.push_back({i * 10000, -40.0f + 2.0f * i});
    OK(seed(doc, 0, AutomationParam::Volume, ramp));
    OK(drag_lane_range(ctl, 0, 0, 0, 0, 0, 200000, why));
    OK(ctl.thin_automation(why));
    OK(points(AutomationParam::Volume) ==
       (std::vector<AutomationPoint>{{0, -40.0f}, {100000, -20.0f}}));
    // A step inside the range survives: each side of it is thinned alone.
    OK(seed(doc, 0, AutomationParam::Volume,
            {{0, -40.0f}, {10000, -38.0f}, {20000, -36.0f}, {20000, -10.0f}, {30000, -10.0f},
             {40000, -10.0f}, {50000, -10.0f}}));
    OK(ctl.thin_automation(why));
    OK(points(AutomationParam::Volume) ==
       (std::vector<AutomationPoint>{{0, -40.0f}, {20000, -36.0f}, {20000, -10.0f}, {50000, -10.0f}}));
    // Pan has its own tolerance: 0.01 off the line is nothing on a fader in
    // dB, and twice what a pan lane lets go.
    OK(seed(doc, 0, AutomationParam::Volume, {{0, -20.0f}, {10000, -9.99f}, {20000, 0.0f}}));
    OK(seed(doc, 0, AutomationParam::Pan, {{0, -0.5f}, {10000, 0.01f}, {20000, 0.5f}}));
    OK(ctl.thin_automation(why) && points(AutomationParam::Volume).size() == 2);
    OK(drag_lane_range(ctl, 0, 1, 0, 0, 1, 200000, why) && ctl.selection().autolane == 1);
    OK(!ctl.thin_automation(why) && why.find("nothing to thin") != std::string::npos);
    OK(points(AutomationParam::Pan).size() == 3);
    OK(seed(doc, 0, AutomationParam::Pan, {{0, -0.5f}, {10000, 0.004f}, {20000, 0.5f}}));
    OK(ctl.thin_automation(why) && points(AutomationParam::Pan).size() == 2);
    return 0;
}

// Undo and redo change the lane under a picked breakpoint: the pick goes,
// so Clear cannot delete whichever point now has its index (#181 review).
int test_undo_drops_the_picked_point() {
    ModelDocument doc(make_empty_session(1, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    const ViewState& v = ctl.view();
    const auto xf = [&](Frames f) { return static_cast<double>(f) / v.fpp; };
    const double y = v.autolane_y(0, 0, 0.5f);
    for (Frames f : {Frames(120000), Frames(240000), Frames(360000)}) {
        OK(ctl.press(xf(f), y, why) && ctl.release(xf(f), y, why));
    }
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 3);
    const double x0 = xf(lane(doc, 0, AutomationParam::Volume)->points[0].at);
    const double y0 = v.autolane_y(
        0, 0, lane_position(AutomationParam::Volume, lane(doc, 0, AutomationParam::Volume)->points[0].value));
    OK(ctl.press(x0, y0, why) && ctl.release(x0, y0, why));
    OK(ctl.selection().point == 0);
    OK(ctl.undo(why));
    OK(ctl.selection().point == -1);
    const uint64_t r = doc.snapshot().revision;
    OK(!ctl.clear(why) && doc.snapshot().revision == r);
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 2);
    OK(ctl.press(x0, y0, why) && ctl.release(x0, y0, why) && ctl.selection().point == 0);
    OK(ctl.redo(why));
    OK(ctl.selection().point == -1);
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 3);
    return 0;
}

// tool.next (Pro Tools' Escape) reaches every tool, the Pencil included.
int test_tool_cycle() {
    ModelDocument doc(make_empty_session(1, kRate));
    EditController ctl(doc);
    OK(ctl.tool() == Tool::Smart);
    for (Tool t : {Tool::Trim, Tool::Select, Tool::Grab, Tool::Pencil, Tool::Smart}) {
        ctl.next_tool();
        OK(ctl.tool() == t);
    }
    return 0;
}

// Vertical zoom grows every lane above the selection too; the selected
// track, or its selected lane, stays where it was on screen (#192 review).
int test_zoom_keeps_the_selection_still() {
    ModelDocument doc(make_empty_session(8, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 400);
    std::string why;
    for (int i = 0; i < 8; ++i) OK(ctl.toggle_autolanes(i, why));
    ctl.select_track(5);
    ctl.scroll_by(0, ctl.view().y_of_track(5) - 100);
    OK(std::fabs(ctl.view().y_of_track(5) - 100) < 1e-6);
    ctl.vertical_zoom(true);
    OK(std::fabs(ctl.view().y_of_track(5) - 100) < 1e-6);
    ctl.vertical_zoom(false);
    ctl.vertical_zoom(false);
    OK(std::fabs(ctl.view().y_of_track(5) - 100) < 1e-6);
    // A lane selected: that lane is what stays.
    OK(drag_lane_range(ctl, 5, 1, 1000, 5, 1, 1000, why));
    OK(ctl.selection().track == 5 && ctl.selection().autolane == 1);
    const double lane_y = ctl.view().y_of_autolane(5, 1);
    ctl.vertical_zoom(true);
    OK(std::fabs(ctl.view().y_of_autolane(5, 1) - lane_y) < 1e-6);
    // At the top the scroll cannot go above the first track, and stops there.
    ctl.select_track(0);
    ctl.scroll_by(0, -1e9);
    ctl.vertical_zoom(true);
    OK(ctl.view().scroll_y == 0);
    return 0;
}

// A Selector range dragged across tracks covers the same lane on each, and
// Cut, Copy, Paste, Clear and Thin act on every one of them as one undo
// step. A track in the range whose lanes are hidden shows no curve to have
// edited, so it is left alone and a held notice says so.
int test_range_across_tracks() {
    ModelDocument doc(make_empty_session(4, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 900);
    std::string why;
    for (int i : {0, 1, 3}) OK(ctl.toggle_autolanes(i, why));
    const ViewState& v = ctl.view();
    const auto xf = [&](Frames f) { return static_cast<double>(f) / v.fpp; };
    const std::vector<AutomationPoint> shape{
        {0, -20.0f}, {120000, -10.0f}, {240000, 0.0f}, {360000, -30.0f}};
    for (int i = 0; i < 4; ++i) OK(seed(doc, i, AutomationParam::Volume, shape));
    const std::string hidden = doc.snapshot().edit->tracks[2]->name;

    Scene scene;
    class Flat final : public PeakSource {
    public:
        void peaks(Id, uint16_t, Frames, double, int count, PeakPair* out) const override {
            for (int i = 0; i < count; ++i) out[i] = {0.0f, 0.0f};
        }
    } peaks;
    const auto fills = [&]() {
        build_timeline_scene(doc.snapshot(), v, ctl.selection(), ctl.drag(), false,
                             GridValue::Beat, peaks, scene);
        int n = 0;
        for (const SceneRect& r : scene.rects) n += r.argb == theme::kSelectionFill;
        return n;
    };

    // From track 0's Volume lane to track 3's Pan lane: four tracks, the
    // Volume lane on each, drawn on the three that show it.
    ctl.set_tool(Tool::Select);
    OK(ctl.press(xf(60000), v.autolane_y(0, 0, 0.5f), why));
    ctl.move(xf(300000), v.autolane_y(3, 1, 0.5f));
    OK(ctl.drag().track_end == 3);
    OK(fills() == 3);
    OK(ctl.release(xf(300000), v.autolane_y(3, 1, 0.5f), why));
    const Selection& sel = ctl.selection();
    OK(sel.track == 0 && sel.tracks == 4 && sel.autolane == 0);
    OK(sel.start == 60000 && sel.end == 300000);
    OK(fills() == 3);

    // Clear: one undo step over the three shown lanes; the hidden one stays.
    const uint64_t r0 = doc.snapshot().revision;
    OK(ctl.clear(why));
    OK(doc.snapshot().revision == r0 + 1 && doc.undo_label() == "Clear Automation");
    for (int i : {0, 1, 3}) {
        for (const AutomationPoint& p : lane(doc, i, AutomationParam::Volume)->points) {
            OK(p.at <= 60000 || p.at >= 299999);
        }
    }
    OK(lane(doc, 2, AutomationParam::Volume)->points == shape);
    const std::string notice = ctl.take_follow_notice();
    OK(notice.find(hidden) != std::string::npos && notice.find("Clear") != std::string::npos);
    OK(ctl.take_follow_notice().empty());
    OK(ctl.undo(why));
    for (int i = 0; i < 4; ++i) OK(lane(doc, i, AutomationParam::Volume)->points == shape);
    OK(valid(doc.snapshot()));

    // Copy holds one slot a track, the hidden one's empty, so a paste lands
    // each where it came from relative to the first.
    OK(drag_lane_range(ctl, 0, 0, 60000, 3, 0, 300000, why) && ctl.selection().tracks == 4);
    OK(ctl.copy(why));
    const std::vector<AutomationClip>& clips = ctl.automation_clipboard();
    OK(clips.size() == 4 && !clips[0].empty() && !clips[1].empty() && clips[2].empty() &&
       !clips[3].empty());
    OK(ctl.take_follow_notice().find(hidden) != std::string::npos);

    // Pasted on track 0 at 600000: one step, the slots on tracks 0 to 3.
    OK(drag_lane_range(ctl, 0, 0, 600000, 0, 0, 600000, why) && ctl.selection().tracks == 1);
    const uint64_t r1 = doc.snapshot().revision;
    OK(ctl.paste(why));
    OK(doc.snapshot().revision == r1 + 1 && doc.undo_label() == "Paste Automation");
    for (int i : {0, 1, 3}) OK(lane(doc, i, AutomationParam::Volume)->points.back().at == 839999);
    OK(lane(doc, 2, AutomationParam::Volume)->points == shape);
    OK(ctl.selection().start == 600000 && ctl.selection().end == 840000 &&
       ctl.selection().tracks == 4);
    OK(valid(doc.snapshot()));
    OK(ctl.undo(why));

    // Pasted on track 1: slot 1 lands on the hidden track and slot 3 has no
    // track to land on; both are named, the rest goes ahead.
    OK(drag_lane_range(ctl, 1, 0, 600000, 1, 0, 600000, why));
    const uint64_t r2 = doc.snapshot().revision;
    OK(ctl.paste(why));
    OK(doc.snapshot().revision == r2 + 1);
    OK(lane(doc, 1, AutomationParam::Volume)->points.back().at == 839999);
    OK(lane(doc, 2, AutomationParam::Volume)->points == shape);
    const std::string left = ctl.take_follow_notice();
    OK(left.find(hidden) != std::string::npos && left.find("no track") != std::string::npos);
    OK(ctl.undo(why));

    // Cut: every shown lane cleared as one step, held as Copy holds it.
    OK(drag_lane_range(ctl, 0, 0, 60000, 1, 0, 300000, why) && ctl.selection().tracks == 2);
    const uint64_t r3 = doc.snapshot().revision;
    OK(ctl.cut(why));
    OK(doc.snapshot().revision == r3 + 1 && doc.undo_label() == "Cut Automation");
    OK(ctl.automation_clipboard().size() == 2);
    OK(ctl.take_follow_notice().empty());
    OK(ctl.undo(why));

    // Thin over two dense ramps: one step.
    std::vector<AutomationPoint> ramp;
    for (int i = 0; i <= 10; ++i) ramp.push_back({i * 10000, -40.0f + 2.0f * i});
    OK(seed(doc, 0, AutomationParam::Volume, ramp));
    OK(seed(doc, 1, AutomationParam::Volume, ramp));
    OK(drag_lane_range(ctl, 0, 0, 0, 1, 0, 200000, why));
    const uint64_t r4 = doc.snapshot().revision;
    OK(ctl.thin_automation(why));
    OK(doc.snapshot().revision == r4 + 1 && doc.undo_label() == "Thin Automation");
    OK(lane(doc, 0, AutomationParam::Volume)->points.size() == 2);
    OK(lane(doc, 1, AutomationParam::Volume)->points.size() == 2);

    // No breakpoint on any covered lane: refused by name, nothing sent.
    OK(drag_lane_range(ctl, 0, 0, 700000, 3, 0, 1000000, why) && ctl.selection().tracks == 4);
    const uint64_t r5 = doc.snapshot().revision;
    OK(!ctl.clear(why) && why.find("Volume lanes") != std::string::npos);
    OK(!ctl.thin_automation(why) && doc.snapshot().revision == r5);
    return 0;
}

// The Volume Trim lane (ADR-0027 §8 slice 5): the third lane under a track,
// a relative dB in [kMinTrimDb, kMaxTrimDb] on the fader's taper, so 0 dB of
// trim sits where unity sits on the Volume lane. Before slice 5's interface
// it was treated as Pan: a "Gtr Pan" notice and a clamp to [-1, 1].
int test_trim_lane() {
    OK(kLaneCount == 3 && lane_param(2) == AutomationParam::VolumeTrim);
    OK(std::string(lane_name(AutomationParam::VolumeTrim)) == "Volume Trim");
    OK(clamp_lane_value(AutomationParam::VolumeTrim, 2.5f) == 2.5f);
    OK(clamp_lane_value(AutomationParam::VolumeTrim, 30.0f) == kMaxTrimDb);
    OK(clamp_lane_value(AutomationParam::VolumeTrim, -90.0f) == kMinTrimDb);
    OK(clamp_lane_value(AutomationParam::VolumeTrim, std::nanf("")) == 0.0f);
    OK(lane_position(AutomationParam::VolumeTrim, 0.0f) ==
       lane_position(AutomationParam::Volume, 0.0f));
    OK(lane_value(AutomationParam::VolumeTrim, 1.0f) == kMaxTrimDb);
    OK(lane_value(AutomationParam::VolumeTrim, 0.0f) == kMinTrimDb);
    OK(near(lane_value(AutomationParam::VolumeTrim,
                       lane_position(AutomationParam::VolumeTrim, -6.0f)), -6.0f, 0.1f));
    OK(lane_value_text(AutomationParam::VolumeTrim, 2.5f) == "+2.5 dB");
    OK(lane_value_text(AutomationParam::VolumeTrim, kMinTrimDb) == "-60.0 dB");
    OK(thin_tolerance(AutomationParam::VolumeTrim) == thin_tolerance(AutomationParam::Volume));

    // The Pencil on lane 2 writes the trim lane, in its range.
    ModelDocument doc(make_empty_session(3, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 900);
    std::string why;
    for (int i = 0; i < 3; ++i) OK(ctl.toggle_autolanes(i, why));
    const ViewState& v = ctl.view();
    ctl.set_tool(Tool::Pencil);
    OK(ctl.press(100, v.autolane_y(0, 2, 0.99f), why));
    ctl.move(200, v.autolane_y(0, 2, 0.99f));
    OK(ctl.release(300, v.autolane_y(0, 2, 0.99f), why));
    const AutomationLane* drawn = lane(doc, 0, AutomationParam::VolumeTrim);
    OK(drawn != nullptr && lane(doc, 0, AutomationParam::Pan) == nullptr);
    for (const AutomationPoint& p : drawn->points) OK(p.value <= kMaxTrimDb && p.value > 6.0f);
    OK(valid(doc.snapshot()));

    // The Selector across tracks on the trim lane (#265): one Clear over all
    // of them, and a refusal that names the lane.
    const std::vector<AutomationPoint> shape{{0, -20.0f}, {120000, -6.0f}, {240000, 3.0f}};
    for (int i = 0; i < 3; ++i) OK(seed(doc, i, AutomationParam::VolumeTrim, shape));
    OK(drag_lane_range(ctl, 0, 2, 60000, 2, 2, 200000, why));
    OK(ctl.selection().tracks == 3 && ctl.selection().autolane == 2);
    const uint64_t r0 = doc.snapshot().revision;
    OK(ctl.clear(why));
    OK(doc.snapshot().revision == r0 + 1 && doc.undo_label() == "Clear Automation");
    for (int i = 0; i < 3; ++i) {
        for (const AutomationPoint& p : lane(doc, i, AutomationParam::VolumeTrim)->points) {
            OK(p.at <= 60000 || p.at >= 199999);
        }
    }
    OK(ctl.take_follow_notice().empty());
    OK(drag_lane_range(ctl, 0, 2, 700000, 2, 2, 900000, why));
    OK(!ctl.clear(why) && why.find("Volume Trim lanes") != std::string::npos);
    OK(why.find("Pan") == std::string::npos);
    return 0;
}

int test_scene() {
    ModelDocument doc(make_empty_session(2, kRate));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    std::string why;
    OK(ctl.toggle_autolanes(0, why));
    Scene scene;
    class Flat final : public PeakSource {
    public:
        void peaks(Id, uint16_t, Frames, double, int count, PeakPair* out) const override {
            for (int i = 0; i < count; ++i) out[i] = {0.0f, 0.0f};
        }
    } peaks;
    const ViewState& v = ctl.view();
    const auto count_colour = [&](uint32_t c) {
        int n = 0;
        for (const SceneRect& r : scene.rects) n += r.argb == c;
        return n;
    };
    const auto build = [&]() {
        build_timeline_scene(doc.snapshot(), v, ctl.selection(), ctl.drag(), false, GridValue::Beat, peaks, scene);
    };
    // No breakpoint: the static fader, dashed.
    build();
    const int dashes = count_colour(theme::kAutomationVolume);
    OK(dashes > 20 && dashes < 200);
    // The lane's rows are painted where track 1 would otherwise start.
    const SceneRect* bg = rect_at(scene.rects, 500, static_cast<float>(v.y_of_autolane(0, 1) + 5));
    OK(bg != nullptr);
    // A curve is a column a pixel.
    OK(seed(doc, 0, AutomationParam::Volume, {{0, -20.0f}, {240000, 0.0f}}));
    build();
    OK(count_colour(theme::kAutomationVolume) > 200);
    // Off plays the static fader: the lane is drawn as not played.
    TrackMix off = *doc.snapshot().mix.find(doc.snapshot().edit->tracks[0]->id);
    off.automation = AutomationMode::Off;
    OK(doc.submit(SetMix{off}, why));
    build();
    OK(count_colour(theme::kAutomationVolume) == 0);
    // The breakpoint in the hand is drawn where the release leaves it.
    off.automation = AutomationMode::Read;
    OK(doc.submit(SetMix{off}, why));
    const int before = count_colour(theme::kPlayhead);
    OK(ctl.press(300, v.autolane_y(0, 0, 0.9f), why));
    ctl.move(310, v.autolane_y(0, 0, 0.9f));
    build();
    OK(count_colour(theme::kPlayhead) > before);
    OK(ctl.release(310, v.autolane_y(0, 0, 0.9f), why));
    // The trim lane has a colour of its own, dashed at 0 dB with no lane.
    OK(count_colour(theme::kAutomationTrim) > 20);
    // Track 1's row starts below the lanes.
    const SceneRect* row = rect_at(scene.rects, 500, static_cast<float>(v.y_of_track(1) + 5));
    OK(row != nullptr && row->argb != theme::kSunken);
    return 0;
}

}  // namespace

int main() {
    // Every test runs, so one failure does not hide the next.
    const int failed = test_values() + test_wide_plugin_lane() + test_operations() +
                       test_stroke() + test_clip() + test_rows() + test_lane_height_follows_zoom() + test_gestures() +
                       test_range_commands() + test_follow_ops() + test_follow_at_the_lane_cap() +
                       test_thin_exact() + test_undo_drops_the_picked_point() + test_tool_cycle() +
                       test_zoom_keeps_the_selection_still() + test_range_across_tracks() +
                       test_trim_lane() + test_scene();
    if (failed != 0) {
        std::fprintf(stderr, "ui automation: %d tests failed\n", failed);
        return 1;
    }
    std::printf("ui automation: %d checks passed\n", checks);
    return 0;
}
