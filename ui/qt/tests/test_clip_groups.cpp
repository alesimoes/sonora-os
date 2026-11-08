// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip groups, clip looping and sample edits in the interface (ADR-0052): the
// controller's group, ungroup, loop and unloop, each one undo step; a group
// moving as one; the commands with their refusals; the sample edit request the
// selection becomes; and the scene marking grouped clips and loop repeats.
// The sample view's contract (ADR-0096): SampleSource reads and the Pencil's
// stroke turned into the points of one sample.edit draw.
#include <QGuiApplication>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <string>

#include "edit_controller.h"
#include "peak_source.h"
#include "sample_view.h"
#include "session_model.h"
#include "timeline_scene.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;
constexpr Frames kBar = static_cast<Frames>(kRate) * 2;

const Playlist& lane(const ModelDocument& d, std::size_t row) {
    const TrackEdit& t = *d.snapshot().edit->tracks[row];
    for (const auto& p : t.playlists)
        if (p->id == t.active_playlist) return *p;
    return *t.playlists.front();
}

// Row 0 (Kick) holds three clips: bars 0-8, 9-16 and 19-25. Row 15 holds
// two, at bars 12-16 and 20-24, with room after them.
int test_group_ungroup_and_move() {
    ModelDocument doc(make_demo_session(16, kRate));
    EditController c(doc);
    std::string why;
    const Id a = lane(doc, 0).regions[0].id, b = lane(doc, 0).regions[1].id,
             d = lane(doc, 0).regions[2].id;

    CHECK(!c.group_clips(why) && why.find("drag the Selector") != std::string::npos);
    c.select_range(0, kBar, 2 * kBar);
    CHECK(!c.group_clips(why) && why.find("two or more") != std::string::npos);
    c.select_range(0, kBar, 12 * kBar);
    CHECK(c.group_clips(why));
    CHECK(lane(doc, 0).regions[0].clip_group == a && lane(doc, 0).regions[1].clip_group == a);
    CHECK(lane(doc, 0).regions[2].clip_group == 0);
    CHECK(doc.undo_label() == "Clip Group" || doc.undo_label() == "SetClipGroup");

    // A clip in a group cannot be grouped again.
    c.select_range(0, 0, 20 * kBar);
    CHECK(!c.group_clips(why) && why.find("already in a clip group") != std::string::npos);

    // Nudging one clip carries the other the same distance.
    CHECK(c.select_region(b));
    const Frames pa = lane(doc, 0).regions[0].position, pb = lane(doc, 0).regions[1].position;
    CHECK(c.nudge(1, why));
    const Frames da = lane(doc, 0).regions[0].position - pa;
    CHECK(da != 0 && lane(doc, 0).regions[1].position - pb == da);
    CHECK(doc.undo_label() == "Move Group");
    CHECK(c.undo(why));
    CHECK(lane(doc, 0).regions[0].position == pa && lane(doc, 0).regions[1].position == pb);
    // Backwards the group stops at the session start rather than being refused.
    CHECK(c.select_region(a));
    CHECK(c.nudge(-1, why) || why.find("session start") != std::string::npos);
    CHECK(lane(doc, 0).regions[0].position >= 0);

    // Ungroup frees both, as one step.
    CHECK(c.select_region(b));
    CHECK(c.ungroup_clip(why));
    CHECK(lane(doc, 0).regions[0].clip_group == 0 && lane(doc, 0).regions[1].clip_group == 0);
    CHECK(!c.ungroup_clip(why) && why.find("not in a clip group") != std::string::npos);
    CHECK(c.select_region(d));
    CHECK(!c.ungroup_clip(why));
    return 0;
}

int test_loop_and_unloop() {
    ModelDocument doc(make_demo_session(16, kRate));
    EditController c(doc);
    std::string why;
    const Id d = lane(doc, 0).regions[2].id;
    const std::size_t clips = lane(doc, 0).regions.size();

    CHECK(!c.loop_clip(2, why) && why.find("select a region") != std::string::npos);
    CHECK(c.select_region(d));
    CHECK(!c.loop_clip(0, why));
    CHECK(c.loop_clip(2, why));
    const Playlist& p = lane(doc, 0);
    CHECK(p.regions.size() == clips + 2);
    const Region& origin = p.regions[2];
    CHECK(origin.id == d && origin.clip_group == d && origin.loop == 0);
    CHECK(p.regions[3].loop == 1 && p.regions[3].position == origin.position + origin.length &&
          p.regions[3].clip_group == d && p.regions[3].source_offset == origin.source_offset);
    CHECK(p.regions[4].loop == 2 && p.regions[4].position == origin.position + 2 * origin.length);
    CHECK(doc.undo_label() == "Loop Clip");
    CHECK(c.loop_count() == 2);

    // Looping again lays the new count, not more on top of the old.
    CHECK(c.loop_clip(1, why));
    CHECK(lane(doc, 0).regions.size() == clips + 1);
    CHECK(lane(doc, 0).regions[3].loop == 1);
    CHECK(c.unloop_clip(why));
    CHECK(lane(doc, 0).regions.size() == clips);
    CHECK(lane(doc, 0).regions[2].clip_group == 0);
    CHECK(!c.unloop_clip(why) && why.find("not looped") != std::string::npos);
    // The whole loop is one undo step each way.
    CHECK(c.undo(why));
    CHECK(lane(doc, 0).regions.size() == clips + 1);
    CHECK(c.undo(why) && c.undo(why));
    CHECK(lane(doc, 0).regions.size() == clips && lane(doc, 0).regions[2].clip_group == 0);

    // A group loops as a whole, each repeat keeping the gap between its clips.
    const Id first = lane(doc, 15).regions[0].id;
    const Frames p0 = lane(doc, 15).regions[0].position, p1 = lane(doc, 15).regions[1].position;
    const Frames end = lane(doc, 15).regions[1].end();
    c.select_range(15, 0, 30 * kBar);
    CHECK(c.group_clips(why));
    CHECK(c.select_region(first));
    CHECK(c.loop_clip(1, why));
    const Playlist& q = lane(doc, 15);
    CHECK(q.regions.size() == 4);
    CHECK(q.regions[2].loop == 1 && q.regions[2].position == p0 + (end - p0));
    CHECK(q.regions[3].loop == 1 && q.regions[3].position == p1 + (end - p0));
    CHECK(q.regions[2].clip_group == first && q.regions[0].clip_group == first);
    // The group, repeats included, moves as one.
    CHECK(c.select_region(q.regions[3].id));
    const Frames before = q.regions[0].position;
    CHECK(c.nudge(1, why));
    CHECK(lane(doc, 15).regions[0].position != before);
    CHECK(lane(doc, 15).regions[3].position - lane(doc, 15).regions[2].position == p1 - p0);
    return 0;
}

int test_sample_edit_request() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why, arg;
    const Region r = lane(doc, 0).regions[0];
    CHECK(!c.sample_edit_argument("repair", "", arg, why) &&
          why.find("select the samples") != std::string::npos);
    c.select_range(0, r.position + 100, r.position + 164);
    CHECK(c.sample_edit_argument("repair", "", arg, why));
    CHECK(arg == "repair " + std::to_string(r.id) + " " + std::to_string(r.position + 100) + " " +
                     std::to_string(r.position + 164) + " all");
    CHECK(c.sample_edit_argument("gain", "-3", arg, why) && arg.size() > 3 &&
          arg.substr(arg.size() - 2) == "-3");
    // Across the cut between two clips there is no one clip to edit.
    c.select_range(0, r.end() - 10, r.end() + 10);
    CHECK(!c.sample_edit_argument("silence", "", arg, why) && why.find("inside one clip") != std::string::npos);
    // Where no clip is.
    c.select_range(0, r.end() + 1000, r.end() + 1100);
    CHECK(!c.sample_edit_argument("silence", "", arg, why));
    return 0;
}

int test_commands() {
    ModelDocument doc(make_demo_session(16, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    std::string why;
    CommandContext none, arg;
    EditController& c = session.controller();
    const Region r = lane(doc, 0).regions[2];

    CHECK(local.dispatch("clip.group", none, why) == DispatchResult::Failed);
    CHECK(local.dispatch("clip.ungroup", none, why) == DispatchResult::Failed);
    CHECK(local.dispatch("clip.unloop", none, why) == DispatchResult::Failed);
    CHECK(c.select_region(r.id));
    arg.argument = "many";
    CHECK(local.dispatch("clip.loop", arg, why) == DispatchResult::Failed);
    CHECK(why.find("not a number of repeats") != std::string::npos);
    arg.argument = "2";
    CHECK(local.dispatch("clip.loop", arg, why) == DispatchResult::Ok);
    CHECK(lane(doc, 0).regions.size() == 5);
    CHECK(local.dispatch("clip.unloop", none, why) == DispatchResult::Ok);
    CHECK(lane(doc, 0).regions.size() == 3);
    // No argument loops the count last used.
    CHECK(local.dispatch("clip.loop", none, why) == DispatchResult::Ok);
    CHECK(lane(doc, 0).regions.size() == 5);

    // Sample edits go to the engine as sample.edit, built from the selection.
    std::vector<std::pair<QString, QString>> sent;
    c.select_range(0, r.position + 100, r.position + 164);
    CHECK(local.dispatch("sample.repair", none, why) == DispatchResult::Failed);
    CHECK(why.find("no engine") != std::string::npos);
    session.setCommandSender([&](const QString& command, const QString& argument) {
        sent.emplace_back(command, argument);
    });
    CHECK(local.dispatch("sample.repair", none, why) == DispatchResult::Ok);
    CHECK(sent.size() == 1 && sent[0].first == "sample.edit" &&
          sent[0].second.startsWith("repair " + QString::number(r.id) + " "));
    CHECK(local.dispatch("sample.reverse", none, why) == DispatchResult::Ok);
    CHECK(sent.back().second.startsWith("reverse "));
    CHECK(local.dispatch("sample.gain", none, why) == DispatchResult::Failed);
    arg.argument = "-3 dB";
    CHECK(local.dispatch("sample.gain", arg, why) == DispatchResult::Ok);
    CHECK(sent.back().second.endsWith(" all -3"));
    arg.argument = "loud";
    CHECK(local.dispatch("sample.gain", arg, why) == DispatchResult::Failed);
    CHECK(local.dispatch("sample.normalize", none, why) == DispatchResult::Ok);
    CHECK(sent.back().second.endsWith(" all"));
    arg.argument = "1:0,9:0.5";
    CHECK(local.dispatch("sample.draw", arg, why) == DispatchResult::Ok);
    CHECK(sent.back().second.endsWith(" all 1:0,9:0.5"));
    CHECK(local.dispatch("sample.draw", none, why) == DispatchResult::Failed);
    arg.argument = "1";
    CHECK(local.dispatch("sample.silence", arg, why) == DispatchResult::Failed);
    const std::size_t count = sent.size();
    // Nothing selected, nothing sent.
    c.select_range(0, r.end() - 10, r.end() + 10);
    CHECK(local.dispatch("sample.invert", none, why) == DispatchResult::Failed);
    CHECK(sent.size() == count);
    return 0;
}

// Grouped clips wear a bar along their foot, a repeat says which one it is.
int test_scene_marks_groups() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    SyntheticPeakSource peaks(kRate, 120.0);
    c.set_viewport(1200, 400);
    const ViewState v = c.view();
    const auto count = [&] {
        Scene s;
        build_timeline_scene(doc.snapshot(), v, c.selection(), Drag{}, false, GridValue{}, peaks, s);
        return std::make_pair(s.rects.size(), s.labels);
    };
    const auto flat = count();
    CHECK(c.select_region(lane(doc, 0).regions[0].id));
    CHECK(c.loop_clip(1, why));
    const auto looped = count();
    CHECK(looped.first > flat.first);
    bool shown = false;
    for (const auto& l : looped.second) shown = shown || l.text.find("loop 1") != std::string::npos;
    CHECK(shown);
    return 0;
}


// The fake answers every read from memory, deterministically, and zeros
// outside the source; a source still loading says so.
int test_sample_source_fake() {
    FakeSampleSource ramp(FakeSampleSource::Shape::Ramp, 100, 1.0f, 1000);
    float buf[8] = {};
    ramp.read(1, 0, 0, 4, buf);
    CHECK(buf[0] == -1.0f && buf[1] > buf[0] && buf[2] > buf[1] && buf[3] > buf[2]);
    ramp.read(1, 0, 50, 1, buf);
    CHECK(std::fabs(buf[0]) < 1e-6f);
    ramp.read(1, 0, 998, 4, buf);
    CHECK(buf[1] != 0.0f && buf[2] == 0.0f && buf[3] == 0.0f);
    ramp.read(1, 0, -2, 2, buf);
    CHECK(buf[0] == 0.0f && buf[1] == 0.0f);
    // Channels differ, so a stereo view shows two lanes.
    CHECK(ramp.at(0, 10) != ramp.at(1, 10));
    FakeSampleSource sine(FakeSampleSource::Shape::Sine, 40, 0.5f);
    CHECK(std::fabs(sine.at(0, 10) - 0.5f) < 1e-5f && std::fabs(sine.at(0, 0)) < 1e-6f);
    const SampleSource& s = sine;
    CHECK(s.ready(7));
    sine.set_not_ready(7);
    CHECK(!s.ready(7) && s.ready(8));
    // The thresholds the view and the Pencil share, and the engine's limits.
    CHECK(kSampleDotsMaxFpp < 1.0 && kSampleViewMaxFpp == 4.0);
    CHECK(kPencilMaxPoints == 4096 && kPencilMaxFrames == 16'000'000);
    return 0;
}

// One point per pixel column, none skipped between two far-apart pointer
// positions, in increasing frame order.
int test_pencil_interpolates() {
    PencilFrame v;
    v.scroll = 1000.0;
    v.fpp = 2.0;
    v.clip_first = 0;
    v.clip_end = 100000;
    PencilStroke out;
    std::string why;
    CHECK(pencil_points({{10.0, 0.0f}, {14.0, 1.0f}}, v, out, why));
    CHECK(out.points == "1020:0,1022:0.25,1024:0.5,1026:0.75,1028:1");
    CHECK(out.from == 1020 && out.to == 1029 && out.count == 5);
    // Right to left comes out in frame order all the same.
    CHECK(pencil_points({{14.0, 1.0f}, {10.0, 0.0f}}, v, out, why));
    CHECK(out.points == "1020:0,1022:0.25,1024:0.5,1026:0.75,1028:1");
    // Going back over a column redraws it with the later level.
    CHECK(pencil_points({{10.0, 0.5f}, {11.0, 0.5f}, {10.0, -0.5f}}, v, out, why));
    CHECK(out.points == "1020:-0.5,1022:0.5");
    // Zoomed past a frame a pixel, columns sharing a frame give one point.
    v.fpp = 0.5;
    CHECK(pencil_points({{0.0, 0.1f}, {3.0, 0.4f}}, v, out, why));
    CHECK(out.count <= 3 && out.from == 1000 && out.to == 1003);
    CHECK(out.points.find("1001:") != std::string::npos);
    // A level is written with a decimal point whatever the locale.
    CHECK(out.points.rfind("1000:0.1,", 0) == 0);
    return 0;
}

// Levels clamp to -1..1; columns off the clip draw nothing.
int test_pencil_clamps() {
    PencilFrame v;
    v.scroll = 0.0;
    v.fpp = 1.0;
    v.clip_first = 5;
    v.clip_end = 8;
    PencilStroke out;
    std::string why;
    CHECK(pencil_points({{0.0, 0.0f}, {5.0, 3.0f}, {7.0, -3.0f}}, v, out, why));
    CHECK(out.from == 5 && out.to == 8 && out.count == 3);
    CHECK(out.points == "5:1,6:0,7:-1");
    CHECK(pencil_points({{6.0, 2.0f}}, v, out, why) && out.points == "6:1");
    CHECK(pencil_points({{6.0, -0.000001f}}, v, out, why) && out.points == "6:-0.000001");
    return 0;
}

// Nothing to send is refused with a reason, never sent empty.
int test_pencil_refuses() {
    PencilFrame v;
    v.fpp = 1.0;
    v.clip_first = 100;
    v.clip_end = 200;
    PencilStroke out;
    std::string why;
    CHECK(!pencil_points({}, v, out, why) && why.find("nothing drawn") != std::string::npos);
    CHECK(out.points.empty() && out.count == 0);
    // Wholly off the clip.
    CHECK(!pencil_points({{0.0, 0.5f}, {50.0, 0.5f}}, v, out, why));
    CHECK(why.find("nothing drawn") != std::string::npos);
    // At an overview zoom the Pencil does not draw and says why.
    v.fpp = 4.5;
    CHECK(!pencil_points({{120.0, 0.5f}}, v, out, why) && why.find("zoom in") != std::string::npos);
    v.fpp = 4.0;
    CHECK(pencil_points({{30.0, 0.5f}}, v, out, why) && out.points == "120:0.5");
    // More columns than one draw takes.
    v.fpp = 1.0;
    v.clip_first = 0;
    v.clip_end = 1'000'000;
    CHECK(pencil_points({{0.0, 0.0f}, {4095.0, 0.0f}}, v, out, why) && out.count == 4096);
    CHECK(!pencil_points({{0.0, 0.0f}, {4096.0, 0.0f}}, v, out, why));
    CHECK(why.find("4096 points") != std::string::npos);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    int failed = 0;
    failed += test_group_ungroup_and_move();
    failed += test_loop_and_unloop();
    failed += test_sample_edit_request();
    failed += test_commands();
    failed += test_scene_marks_groups();
    failed += test_sample_source_fake();
    failed += test_pencil_interpolates();
    failed += test_pencil_clamps();
    failed += test_pencil_refuses();
    if (failed == 0) std::printf("ui_clip_groups: ok\n");
    return failed == 0 ? 0 : 1;
}
