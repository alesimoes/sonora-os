// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation lanes in the Edit window (ADR-0027, BACKLOG `automation.lanes`),
// without a window: the operations a gesture is built into, the controller's
// mouse and Edit-command paths, the rows lanes make taller, and what the
// timeline draws. Every edit must be one EditAutomation, so one undo step.
#include <cmath>
#include <cstdio>
#include <string>

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
    OK(v.y_of_track(1) == h + 2 * v.autolane_height);
    OK(v.content_height(3) == 3 * h + 2 * v.autolane_height);
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
    OK(std::fabs(ctl.view().y_of_track(1) - (ctl.view().track_height + 2 * base * 1.5)) < 1e-6);
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
    ctl.move(xf(300000), y + 2000);   // other tracks and lanes are not reachable
    OK(ctl.release(xf(300000), y + 2000, why));
    OK(ctl.selection().autolane == 0 && ctl.selection().track == 0 && ctl.selection().tracks == 1);
    OK(ctl.selection().start == 60000 && ctl.selection().end == 300000);
    OK(ctl.copy(why));
    OK(doc.snapshot().revision == r0 && !ctl.automation_clipboard().empty());
    OK(ctl.automation_clipboard().param == AutomationParam::Volume);

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
    OK(ctl.automation_clipboard().param == AutomationParam::Pan);
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
    OK(ctl.automation_clipboard().param == AutomationParam::Volume);
    OK(!ctl.clear(why) && why.find("no breakpoint") != std::string::npos);
    OK(doc.snapshot().revision == r1 + 1);
    // A track's own row goes back to the regions: no range, no automation.
    OK(ctl.press(xf(60000), v.y_of_track(0) + 10, why) && ctl.release(xf(60000), v.y_of_track(0) + 10, why));
    OK(ctl.selection().autolane == -1);
    OK(!ctl.clear(why) && why.find("nothing is selected") != std::string::npos);
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
    // Track 1's row starts below the lanes.
    const SceneRect* row = rect_at(scene.rects, 500, static_cast<float>(v.y_of_track(1) + 5));
    OK(row != nullptr && row->argb != theme::kSunken);
    return 0;
}

}  // namespace

int main() {
    if (test_values() || test_operations() || test_stroke() || test_clip() || test_rows() || test_lane_height_follows_zoom() ||
        test_gestures() || test_range_commands() || test_scene()) {
        return 1;
    }
    std::printf("ui automation: %d checks passed\n", checks);
    return 0;
}
