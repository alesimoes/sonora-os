// StateMachineLauncher (ADR-0164): quantised start on the bar, re-trigger in
// a column, scene launch, stop_track, stop_all, Immediate, and the queue's
// overrun count.
#include "check.h"
#include "sonora/launch/state_machine_launcher.h"

#include <vector>

using namespace sonora::launch;

namespace {

// 120 BPM 4/4 at 48 kHz: a bar is 96000 frames.
constexpr std::int64_t kBar = 96000;

struct Event {
    Transition::Kind kind;
    std::uint32_t track;
    std::uint32_t scene;
    ClipId clip;
    std::int64_t frame;
};

// Runs [from, to) in blocks of `block` frames and records every transition
// at its absolute frame.
std::vector<Event> run(Launcher& l, std::int64_t from, std::int64_t to, std::uint32_t block) {
    std::vector<Event> out;
    for (std::int64_t pos = from; pos < to; pos += block) {
        const TransitionList list = l.advance(pos, block);
        for (std::uint32_t i = 0; i < list.count; ++i) {
            const Transition& tr = list[i];
            out.push_back(Event{tr.kind, tr.track, tr.scene, tr.clip, pos + tr.offset});
        }
    }
    return out;
}

StateMachineLauncher make(Quantize q = Quantize::Bar) {
    LauncherConfig c;
    c.tracks = 3;
    c.scenes = 4;
    c.queue_capacity = 64;
    c.quantize = q;
    return StateMachineLauncher(c);
}

ClipId clip_of(std::uint32_t track, std::uint32_t scene) { return 100 + track * 10 + scene; }

void fill(Launcher& l) {
    for (std::uint32_t t = 0; t < 3; ++t)
        for (std::uint32_t s = 0; s < 4; ++s) l.assign_clip(t, s, clip_of(t, s));
}

int playing_count(const LaunchGrid& g, std::uint32_t track) {
    int n = 0;
    for (std::uint32_t s = 0; s < g.scenes(); ++s)
        if (g.cell(track, s).state == CellState::Playing) ++n;
    return n;
}

int test_quantised_start_on_bar() {
    StateMachineLauncher l = make();
    fill(l);
    l.advance(0, 512);  // applies the assignments
    CHECK(l.trigger_cell(1, 2));
    CHECK(run(l, 512, 1024, 512).empty());
    CHECK(l.grid().cell(1, 2).state == CellState::Queued);
    const std::vector<Event> ev = run(l, 1024, 2 * kBar, 512);
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == Transition::Kind::Start);
    CHECK(ev[0].track == 1 && ev[0].scene == 2 && ev[0].clip == clip_of(1, 2));
    CHECK(ev[0].frame == kBar);
    CHECK(l.grid().cell(1, 2).state == CellState::Playing);
    return 0;
}

int test_retrigger_replaces_in_column() {
    StateMachineLauncher l = make();
    fill(l);
    l.trigger_cell(0, 0);
    run(l, 0, 512, 512);  // boundary 0 is inside the first block
    CHECK(l.grid().cell(0, 0).state == CellState::Playing);
    l.trigger_cell(0, 3);
    std::vector<Event> ev = run(l, 512, kBar - 512, 512);
    CHECK(ev.empty());
    CHECK(l.grid().cell(0, 0).state == CellState::Playing);
    CHECK(l.grid().cell(0, 3).state == CellState::Queued);
    ev = run(l, kBar - 512, kBar + 512, 512);
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == Transition::Kind::Stop && ev[0].scene == 0 && ev[0].frame == kBar);
    CHECK(ev[1].kind == Transition::Kind::Start && ev[1].scene == 3 && ev[1].frame == kBar);
    CHECK(l.grid().cell(0, 0).state == CellState::Stopped);
    CHECK(l.grid().cell(0, 3).state == CellState::Playing);
    CHECK(playing_count(l.grid(), 0) == 1);
    return 0;
}

int test_second_trigger_replaces_pending() {
    StateMachineLauncher l = make();
    fill(l);
    l.trigger_cell(2, 1);
    l.advance(1, 512);  // after bar 0: lands on bar 1
    l.trigger_cell(2, 2);
    const std::vector<Event> ev = run(l, 513, kBar + 513, 512);
    CHECK(ev.size() == 1);
    CHECK(ev[0].scene == 2 && ev[0].frame == kBar);
    CHECK(l.grid().cell(2, 1).state == CellState::Stopped);
    return 0;
}

int test_scene_and_stop_all() {
    StateMachineLauncher l = make();
    fill(l);
    l.assign_clip(2, 1, kNoClip);  // a gap in the scene
    l.trigger_scene(1);
    std::vector<Event> ev = run(l, 0, 512, 512);
    CHECK(ev.size() == 2);
    CHECK(l.grid().cell(0, 1).state == CellState::Playing);
    CHECK(l.grid().cell(1, 1).state == CellState::Playing);
    CHECK(l.grid().cell(2, 1).state == CellState::Stopped);

    l.trigger_cell(2, 3);  // queued only: stop_all cancels it with no event
    l.stop_all();
    run(l, 512, 1024, 512);
    CHECK(l.grid().cell(0, 1).state == CellState::QueuedStop);
    CHECK(l.grid().cell(1, 1).state == CellState::QueuedStop);
    CHECK(l.grid().cell(2, 3).state == CellState::Stopped);
    ev = run(l, 1024, 2 * kBar, 512);
    CHECK(ev.size() == 2);
    for (const Event& e : ev) {
        CHECK(e.kind == Transition::Kind::Stop);
        CHECK(e.frame == kBar);
        CHECK(e.scene == 1);
    }
    for (std::uint32_t t = 0; t < 3; ++t) CHECK(playing_count(l.grid(), t) == 0);
    return 0;
}

int test_stop_track_keeps_clip_identity() {
    StateMachineLauncher l = make(Quantize::Immediate);
    fill(l);
    l.trigger_cell(0, 2);
    run(l, 0, 256, 256);
    l.assign_clip(0, 2, 999);  // re-pointing the cell does not rename the sounding clip
    l.stop_track(0);
    const std::vector<Event> ev = run(l, 256, 512, 256);
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == Transition::Kind::Stop && ev[0].clip == clip_of(0, 2));
    return 0;
}

int test_immediate() {
    StateMachineLauncher l = make(Quantize::Immediate);
    fill(l);
    l.advance(0, 128);
    l.trigger_cell(1, 0);
    TransitionList list = l.advance(12345, 128);
    CHECK(list.count == 1 && list[0].offset == 0 && list[0].kind == Transition::Kind::Start);
    l.set_quantize(Quantize::Bar);
    l.trigger_cell(1, 1);
    list = l.advance(12473, 128);
    CHECK(list.count == 0);
    CHECK(l.grid().cell(1, 1).state == CellState::Queued);
    return 0;
}

// A command is quantised from the frame it arrived at: a bar line that went
// by while it waited (the drain came late) still lands, at offset 0, rather
// than a whole bar later.
int test_quantised_from_arrival() {
    StateMachineLauncher l = make();
    fill(l);
    l.advance(0, 512);
    CHECK(l.trigger_cell(0, 1));  // arrives at 512: the next bar is kBar
    const TransitionList list = l.advance(kBar + 10, 512);
    CHECK(list.count == 1);
    CHECK(list[0].kind == Transition::Kind::Start && list[0].offset == 0);
    CHECK(l.clock_is_lock_free());
    return 0;
}

int test_queue_overrun_counted() {
    LauncherConfig c;
    c.tracks = 1;
    c.scenes = 1;
    c.queue_capacity = 4;
    StateMachineLauncher l(c);
    int accepted = 0;
    for (int i = 0; i < 10; ++i) accepted += l.stop_all() ? 1 : 0;
    CHECK(accepted == 4);
    CHECK(l.overruns() == 6);
    CHECK(!l.trigger_cell(1, 0));  // outside the grid: refused, not an overrun
    CHECK(l.overruns() == 6);
    l.advance(0, 64);
    CHECK(l.stop_all());
    return 0;
}

}  // namespace

int main() {
    if (test_quantised_start_on_bar()) return 1;
    if (test_retrigger_replaces_in_column()) return 1;
    if (test_second_trigger_replaces_pending()) return 1;
    if (test_scene_and_stop_all()) return 1;
    if (test_stop_track_keeps_clip_identity()) return 1;
    if (test_immediate()) return 1;
    if (test_quantised_from_arrival()) return 1;
    if (test_queue_overrun_counted()) return 1;
    std::printf("launch_launcher: ok\n");
    return 0;
}
