// Record-to-cell (ADR-0164): an armed cell captures from the next quantised
// boundary; a fixed length ends it on the exact frame and plays the new clip,
// an open capture ends on the next command on its column.
#include "check.h"
#include "sonora/launch/state_machine_launcher.h"

#include <vector>

using namespace sonora::launch;

namespace {

// 120 BPM 4/4 at 48 kHz.
constexpr std::int64_t kBar = 96000;
using K = Transition::Kind;

struct Event {
    K kind;
    std::uint32_t scene;
    ClipId clip;
    std::int64_t frame;
};

std::vector<Event> run(Launcher& l, std::int64_t from, std::int64_t to, std::uint32_t block) {
    std::vector<Event> out;
    for (std::int64_t pos = from; pos < to; pos += block) {
        const TransitionList list = l.advance(pos, block);
        for (std::uint32_t i = 0; i < list.count; ++i)
            out.push_back(Event{list[i].kind, list[i].scene, list[i].clip, pos + list[i].offset});
    }
    return out;
}

StateMachineLauncher make() {
    LauncherConfig c;
    c.tracks = 1;
    c.scenes = 2;
    c.quantize = Quantize::Bar;
    return StateMachineLauncher(c);
}

CellState state(const Launcher& l, std::uint32_t scene) { return l.grid().cell(0, scene).state; }

int test_fixed_length_capture_then_plays() {
    StateMachineLauncher l = make();
    l.advance(0, 512);
    CHECK(l.arm_record(0, 0, 77, 2));
    std::vector<Event> ev = run(l, 512, 1024, 512);
    CHECK(ev.empty());
    CHECK(state(l, 0) == CellState::Armed);
    ev = run(l, 1024, kBar + 512, 512);
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == K::RecordStart && ev[0].clip == 77 && ev[0].frame == kBar);
    CHECK(state(l, 0) == CellState::Recording);
    CHECK(l.grid().cell(0, 0).clip == kNoClip);  // not a clip until it ends
    ev = run(l, kBar + 512, 4 * kBar, 512);
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == K::RecordStop && ev[0].clip == 77 && ev[0].frame == 3 * kBar);
    CHECK(ev[1].kind == K::Start && ev[1].clip == 77 && ev[1].frame == 3 * kBar);
    CHECK(l.grid().cell(0, 0).clip == 77);
    CHECK(state(l, 0) == CellState::Playing);
    return 0;
}

int test_open_capture_replaces_playing_and_ends_on_trigger() {
    StateMachineLauncher l = make();
    l.assign_clip(0, 1, 11);
    l.trigger_cell(0, 1);
    run(l, 0, 512, 512);  // plays from boundary 0
    CHECK(l.arm_record(0, 0, 77, 0));
    std::vector<Event> ev = run(l, 512, kBar + 512, 512);
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == K::Stop && ev[0].scene == 1 && ev[0].frame == kBar);
    CHECK(ev[1].kind == K::RecordStart && ev[1].scene == 0 && ev[1].frame == kBar);
    CHECK(state(l, 1) == CellState::Stopped);
    // No fixed length: still capturing bars later.
    ev = run(l, kBar + 512, 5 * kBar + 512, 512);
    CHECK(ev.empty());
    CHECK(state(l, 0) == CellState::Recording);
    l.trigger_cell(0, 0);  // the recording cell: end the capture and play it
    ev = run(l, 5 * kBar + 512, 5 * kBar + 1024, 512);
    CHECK(ev.empty());
    CHECK(state(l, 0) == CellState::Recording);
    ev = run(l, 5 * kBar + 1024, 7 * kBar, 512);
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == K::RecordStop && ev[0].frame == 6 * kBar && ev[0].clip == 77);
    CHECK(ev[1].kind == K::Start && ev[1].frame == 6 * kBar && ev[1].clip == 77);
    CHECK(state(l, 0) == CellState::Playing);
    return 0;
}

int test_stop_ends_capture_without_playing() {
    StateMachineLauncher l = make();
    l.arm_record(0, 0, 77, 0);
    run(l, 0, 512, 512);  // captures from boundary 0
    CHECK(state(l, 0) == CellState::Recording);
    l.stop_track(0);
    const std::vector<Event> ev = run(l, 512, 2 * kBar, 512);
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == K::RecordStop && ev[0].frame == kBar && ev[0].clip == 77);
    CHECK(l.grid().cell(0, 0).clip == 77);
    CHECK(state(l, 0) == CellState::Stopped);
    return 0;
}

int test_arm_refusals() {
    StateMachineLauncher l = make();
    CHECK(!l.arm_record(0, 0, kNoClip, 1));  // a capture needs its region id
    CHECK(!l.arm_record(1, 0, 77, 1));
    CHECK(!l.arm_record(0, 2, 77, 1));
    CHECK(l.overruns() == 0);
    return 0;
}

int test_fixed_end_survives_grid_change() {
    StateMachineLauncher l = make();
    l.set_transport(TransportSnapshot{48000.0, 120.0, 4, 0});
    l.arm_record(0, 0, 77, 1);
    run(l, 0, 512, 512);
    // A locate moves the grid; the capture length is a length, not a bar line.
    l.set_transport(TransportSnapshot{48000.0, 120.0, 4, 1000});
    const std::vector<Event> ev = run(l, 512, 2 * kBar, 512);
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == K::RecordStop && ev[0].frame == kBar);
    return 0;
}

int test_arm_another_cell_while_recording() {
    StateMachineLauncher l = make();
    l.arm_record(0, 0, 77, 0);
    run(l, 0, 512, 512);
    l.arm_record(0, 1, 78, 0);
    const std::vector<Event> ev = run(l, 512, kBar + 512, 512);
    CHECK(ev.size() == 2);
    CHECK(ev[0].kind == K::RecordStop && ev[0].scene == 0 && ev[0].frame == kBar);
    CHECK(ev[1].kind == K::RecordStart && ev[1].scene == 1 && ev[1].clip == 78);
    CHECK(l.grid().cell(0, 0).clip == 77);
    CHECK(state(l, 0) == CellState::Stopped);
    CHECK(state(l, 1) == CellState::Recording);
    return 0;
}

}  // namespace

int main() {
    if (test_fixed_length_capture_then_plays()) return 1;
    if (test_open_capture_replaces_playing_and_ends_on_trigger()) return 1;
    if (test_stop_ends_capture_without_playing()) return 1;
    if (test_arm_refusals()) return 1;
    if (test_fixed_end_survives_grid_change()) return 1;
    if (test_arm_another_cell_while_recording()) return 1;
    std::printf("launch_record: ok\n");
    return 0;
}
