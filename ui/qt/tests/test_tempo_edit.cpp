// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tempo editing in the interface (time.tempo_edit, ADR-0050): the controller's
// set, ramp, scale, meter and remove, each a SetSettings with the map
// replaced; Undo Tempo and Redo Tempo on their own stack; the tempo.edit
// grammar and its refusals; the dialog request; and the tempo track scene.
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>
#include <cstdio>
#include <cstdlib>
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
// One bar of 4/4 at 120 bpm, the demo session's tempo.
constexpr Frames kBar = 96000;

const std::vector<TempoEntry>& tempo(const ModelDocument& d) { return d.snapshot().settings.tempo; }

int test_controller() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    const std::vector<TempoEntry> original = tempo(doc);
    CHECK(original.size() == 1 && original[0].bpm == 120.0f);

    CHECK(c.set_tempo(4 * kBar, 90.0f, why));
    CHECK(tempo(doc).size() == 2 && tempo(doc)[1].bpm == 90.0f && tempo(doc)[1].at == 4 * kBar);
    CHECK(!c.set_tempo(0, 5000.0f, why));
    CHECK(why.find("1 to 1000") != std::string::npos);
    CHECK(tempo(doc).size() == 2);

    // A ramp over bars 5 and 6 from the tempo in force to 120.
    CHECK(c.ramp_tempo(4 * kBar, 6 * kBar, 0.0f, 120.0f, why));
    CHECK(tempo(doc).size() > 6);
    CHECK(tempo(doc)[tempo_index_at(4 * kBar, doc.snapshot().settings)].bpm == 90.0f);
    CHECK(tempo(doc).back().at == 6 * kBar && tempo(doc).back().bpm == 120.0f);

    CHECK(c.set_meter(8 * kBar + 3000, 3, 4, why));
    // The bar line nearest, with the meter held to the end of the map.
    CHECK(std::llabs(c.selection().insertion - (8 * kBar + 3000)) <= kBar / 2);
    CHECK(tempo(doc).back().meter[0] == 3 && tempo(doc).back().at == 6 * kBar);
    CHECK(tempo_at(c.selection().insertion, doc.snapshot().settings).meter[0] == 3);

    // Remove goes to the change nearest the cursor; the first is kept.
    const std::size_t before = tempo(doc).size();
    CHECK(c.remove_tempo(8 * kBar, why));
    CHECK(tempo(doc).size() == before - 1);

    CHECK(c.scale_tempo(0, 2 * kBar, 2.0, why));
    CHECK(tempo(doc)[0].bpm == 240.0f);

    // Undo Tempo walks all the way back, Redo Tempo forward again.
    const std::vector<TempoEntry> now = tempo(doc);
    int steps = 0;
    while (c.can_tempo_undo()) {
        CHECK(c.tempo_undo(why));
        ++steps;
    }
    CHECK(steps == 5);
    CHECK(tempo(doc) == original);
    CHECK(!c.tempo_undo(why));
    CHECK(why.find("nothing to undo") != std::string::npos);
    while (c.can_tempo_redo()) CHECK(c.tempo_redo(why));
    CHECK(tempo(doc) == now);

    // A new edit after an undo drops the redo steps.
    CHECK(c.tempo_undo(why));
    CHECK(c.can_tempo_redo());
    CHECK(c.set_tempo(kBar, 100.0f, why));
    CHECK(!c.can_tempo_redo());

    // A map changed by anyone else clears the history instead of rewinding it.
    SetSettings other{doc.snapshot().settings};
    other.settings.tempo = {TempoEntry{0, 77.0f, {{4, 4}}}};
    CHECK(c.document().submit(Operation{other}, why));
    CHECK(!c.tempo_undo(why));
    CHECK(why.find("changed elsewhere") != std::string::npos);
    CHECK(!c.can_tempo_undo() && !c.can_tempo_redo());
    return 0;
}

int test_commands() {
    ModelDocument doc(make_demo_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    QSignalSpy asked(&session, &SessionModel::tempoRequested);
    EditController& c = session.controller();
    std::string why;
    CommandContext none;

    // Without a text the dialog opens, offering the tempo in force.
    CHECK(local.dispatch("tempo.edit", none, why) == DispatchResult::Ok);
    CHECK(asked.size() == 1 && asked.at(0).at(0).toString() == QStringLiteral("120"));

    CommandContext arg;
    arg.argument = "96";
    c.set_insertion(2 * kBar);
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Ok);
    CHECK(tempo(doc).size() == 2 && tempo(doc)[1].at == 2 * kBar && tempo(doc)[1].bpm == 96.0f);

    // A ramp and a factor need a range.
    arg.argument = "120>90";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Failed);
    CHECK(why.find("select the stretch") != std::string::npos);
    arg.argument = "x2";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Failed);
    c.select_range(0, 4 * kBar, 6 * kBar);
    arg.argument = "120>90";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Ok);
    CHECK(tempo(doc).back().bpm == 90.0f && tempo(doc).back().at == 6 * kBar);
    CHECK(tempo(doc)[tempo_index_at(4 * kBar, doc.snapshot().settings)].bpm == 120.0f);
    arg.argument = ">60";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Ok);
    arg.argument = "x0.5";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Ok);
    arg.argument = "3/4";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Ok);
    CHECK(tempo_at(8 * kBar, doc.snapshot().settings).meter[0] == 3);
    arg.argument = "faster";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Failed);
    CHECK(why.find("type a tempo") != std::string::npos);
    arg.argument = "0.2";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Failed);
    arg.argument = "5/3";
    CHECK(local.dispatch("tempo.edit", arg, why) == DispatchResult::Failed);

    // Tempo undo and redo are commands; the tempo track toggles.
    const std::vector<TempoEntry> now = tempo(doc);
    CHECK(local.dispatch("tempo.undo", none, why) == DispatchResult::Ok);
    CHECK(tempo(doc) != now);
    CHECK(local.dispatch("tempo.redo", none, why) == DispatchResult::Ok);
    CHECK(tempo(doc) == now);
    QSignalSpy shown(&session, &SessionModel::tempoTrackChanged);
    CHECK(!session.tempoTrackShown());
    CHECK(local.dispatch("tempo.track", none, why) == DispatchResult::Ok);
    CHECK(session.tempoTrackShown() && shown.size() == 1);
    return 0;
}

// The tempo track draws the map as a line: a ramp's steps make more marks than
// the same session with one tempo, and its handles carry the bpm.
int test_track_scene() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    c.set_viewport(1200, 400);
    const ViewState v = c.view();
    Scene flat, ramped;
    build_conductor_ruler_scene(ConductorRuler::Track, doc.snapshot().settings, v, 56, flat);
    CHECK(!flat.rects.empty());
    CHECK(c.set_tempo(2 * kBar, 90.0f, why));
    CHECK(c.ramp_tempo(2 * kBar, 4 * kBar, 90.0f, 140.0f, why));
    build_conductor_ruler_scene(ConductorRuler::Track, doc.snapshot().settings, v, 56, ramped);
    CHECK(ramped.rects.size() > flat.rects.size() + 6);
    bool top = false, bottom = false;
    for (const auto& l : ramped.labels) {
        top = top || l.text.find("140") != std::string::npos;
        bottom = bottom || l.text.find("90") != std::string::npos;
    }
    CHECK(top && bottom);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    int failed = 0;
    failed += test_controller();
    failed += test_commands();
    failed += test_track_scene();
    if (failed == 0) std::printf("ui_tempo_edit: ok\n");
    return failed == 0 ? 0 : 1;
}
