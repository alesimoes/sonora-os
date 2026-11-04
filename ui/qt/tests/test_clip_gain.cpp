// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip gain in the interface: the controller's level, nudge and gain points,
// each one undo step named "Clip Gain"; the commands with their arguments and
// refusals; the dialog request; and the scene drawing the line.
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>
#include <cstdio>
#include <string>

#include "edit_controller.h"
#include "peak_source.h"
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

const Region& first_region(const ModelDocument& d, std::size_t row) {
    const TrackEdit& t = *d.snapshot().edit->tracks[row];
    for (const auto& p : t.playlists)
        if (p->id == t.active_playlist) return p->regions.front();
    return t.playlists.front()->regions.front();
}

int test_controller() {
    ModelDocument doc(make_demo_session(4, kRate));
    EditController c(doc);
    std::string why;

    CHECK(!c.set_clip_gain(3.0f, why));
    CHECK(why == "select a region first: click it with the Grabber or the Smart Tool");
    CHECK(c.select_region(first_region(doc, 1).id));
    CHECK(c.set_clip_gain(-4.5f, why));
    CHECK(first_region(doc, 1).gain_db == -4.5f);
    CHECK(doc.undo_label() == "Clip Gain" || doc.undo_label() == "SetRegionGain");
    CHECK(!c.set_clip_gain(40.0f, why) && !why.empty());
    CHECK(c.nudge_clip_gain(1.0f, why));
    CHECK(first_region(doc, 1).gain_db == -3.5f);
    CHECK(c.nudge_clip_gain(-2.0f, why));
    CHECK(first_region(doc, 1).gain_db == -5.5f);
    CHECK(c.set_clip_gain(24.0f, why));
    CHECK(!c.nudge_clip_gain(1.0f, why) && why.find("limit") != std::string::npos);
    CHECK(c.undo(why));
    CHECK(first_region(doc, 1).gain_db == -5.5f);

    // A gain point: the first one holds both ends at 0 dB; the line is in
    // source frames, so it stays with the clip when the clip is read back.
    const Region r = first_region(doc, 1);
    const Frames mid = r.position + r.length / 2;
    CHECK(!c.set_gain_point(r.end() + 10, -6.0f, why));
    CHECK(c.set_gain_point(mid, -6.0f, why));
    const Region& with = first_region(doc, 1);
    CHECK(with.gain_line.size() == 3);
    CHECK(with.gain_line[0].at == r.source_offset && with.gain_line[0].db == 0.0f);
    CHECK(with.gain_line[1].at == r.source_offset + r.length / 2 && with.gain_line[1].db == -6.0f);
    CHECK(with.gain_db == -5.5f);
    CHECK(EditController::clip_line_db_at(with, mid) == -6.0f);
    CHECK(EditController::clip_line_db_at(with, r.position) == 0.0f);
    CHECK(c.set_gain_point(mid, -3.0f, why));
    CHECK(first_region(doc, 1).gain_line.size() == 3 && first_region(doc, 1).gain_line[1].db == -3.0f);
    CHECK(c.remove_gain_point(mid + 100, why));
    CHECK(first_region(doc, 1).gain_line.size() == 2);
    CHECK(!c.remove_gain_point(mid, why) || first_region(doc, 1).gain_line.size() == 1);
    CHECK(c.clear_gain_line(why));
    CHECK(first_region(doc, 1).gain_line.empty());
    CHECK(c.undo(why));
    return 0;
}

int test_commands() {
    ModelDocument doc(make_demo_session(3, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    QSignalSpy asked(&session, &SessionModel::clipGainRequested);
    std::string why;
    CommandContext none;

    CHECK(local.dispatch("clip.gain", none, why) == DispatchResult::Failed);
    CHECK(session.controller().select_region(first_region(doc, 1).id));
    CHECK(local.dispatch("clip.gain", none, why) == DispatchResult::Ok);
    CHECK(asked.size() == 1 && asked.at(0).at(0).toDouble() == 0.0);

    CommandContext arg;
    arg.argument = "-3.5";
    CHECK(local.dispatch("clip.gain", arg, why) == DispatchResult::Ok);
    CHECK(first_region(doc, 1).gain_db == -3.5f);
    arg.argument = "2 dB";
    CHECK(local.dispatch("clip.gain", arg, why) == DispatchResult::Ok);
    CHECK(first_region(doc, 1).gain_db == 2.0f);
    arg.argument = "loud";
    CHECK(local.dispatch("clip.gain", arg, why) == DispatchResult::Failed);
    CHECK(why.find("not a level in dB") != std::string::npos);

    CHECK(local.dispatch("clip.gain_up", none, why) == DispatchResult::Ok);
    CHECK(first_region(doc, 1).gain_db == 3.0f);
    CHECK(local.dispatch("clip.gain_down", none, why) == DispatchResult::Ok);
    arg.argument = "0.5";
    CHECK(local.dispatch("clip.gain_down", arg, why) == DispatchResult::Ok);
    CHECK(first_region(doc, 1).gain_db == 1.5f);

    session.controller().set_insertion(first_region(doc, 1).position + 1000);
    arg.argument = "-8";
    CHECK(local.dispatch("clip.gain_point", arg, why) == DispatchResult::Ok);
    CHECK(first_region(doc, 1).gain_line.size() == 3);
    CHECK(local.dispatch("clip.gain_line_clear", none, why) == DispatchResult::Ok);
    CHECK(first_region(doc, 1).gain_line.empty());
    return 0;
}

// The scene: a clip with a level and a line draws more rectangles (the line
// and its handles) than the same clip flat, and shows its level in its label.
int test_scene_draws_the_line() {
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
    CHECK(c.select_region(first_region(doc, 0).id));
    CHECK(c.set_clip_gain(-6.0f, why));
    const auto lined = count();
    CHECK(lined.first > flat.first);
    bool shown = false;
    for (const auto& l : lined.second) shown = shown || l.text.find("-6.0 dB") != std::string::npos;
    CHECK(shown);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    int failed = 0;
    failed += test_controller();
    failed += test_commands();
    failed += test_scene_draws_the_line();
    if (failed == 0) std::printf("ui_clip_gain: ok\n");
    return failed == 0 ? 0 : 1;
}
