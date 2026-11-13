// Bounce to the arrangement (ADR-0164): the control-side mirror learns clips
// from assignments and from forwarded RecordStop transitions, and a bounce
// turns a scene or a cell into placements on the timeline.
#include "check.h"
#include "sonora/launch/launch_bounce.h"
#include "sonora/launch/launch_mirror.h"
#include "sonora/launch/state_machine_launcher.h"

using namespace sonora::launch;

namespace {

constexpr std::int64_t kBar = 96000;

int test_bounce_scene_skips_empty_cells() {
    LaunchMirror m(3, 2);
    m.assign(0, 1, 10);
    m.assign(2, 1, 12);
    const std::vector<Placement> p = bounce_scene(m.grid(), 1, 4 * kBar, 2 * kBar);
    CHECK(p.size() == 2);
    CHECK(p[0].track == 0 && p[0].clip == 10 && p[0].scene == 1);
    CHECK(p[1].track == 2 && p[1].clip == 12);
    for (const Placement& x : p) CHECK(x.start == 4 * kBar && x.length == 2 * kBar);
    CHECK(bounce_scene(m.grid(), 0, 0, kBar).empty());
    CHECK(bounce_scene(m.grid(), 2, 0, kBar).empty());   // out of the grid
    CHECK(bounce_scene(m.grid(), 1, 0, 0).empty());      // nothing to fill
    return 0;
}

int test_bounce_cell() {
    LaunchMirror m(2, 2);
    m.assign(1, 0, 21);
    std::vector<Placement> p = bounce_cell(m.grid(), 1, 0, 1000, kBar);
    CHECK(p.size() == 1);
    CHECK(p[0].track == 1 && p[0].scene == 0 && p[0].clip == 21);
    CHECK(p[0].start == 1000 && p[0].length == kBar);
    CHECK(bounce_cell(m.grid(), 0, 0, 0, kBar).empty());
    CHECK(bounce_cell(m.grid(), 2, 0, 0, kBar).empty());
    return 0;
}

// The whole path: a capture on the audio side, its transitions forwarded to
// the mirror, then a bounce of the scene it landed in.
int test_recorded_cell_bounces() {
    LauncherConfig c;
    c.tracks = 2;
    c.scenes = 1;
    StateMachineLauncher l(c);
    LaunchMirror m(2, 1);
    l.assign_clip(0, 0, 5);
    m.assign(0, 0, 5);
    l.trigger_scene(0);         // track 1's cell is empty: no start there
    l.arm_record(1, 0, 99, 1);  // ...so the arm replaces nothing
    for (std::int64_t pos = 0; pos < 3 * kBar; pos += 1000) {
        const TransitionList list = l.advance(pos, 1000);
        for (std::uint32_t i = 0; i < list.count; ++i) m.apply(list[i]);
    }
    CHECK(m.grid().cell(1, 0).clip == 99);
    CHECK(m.grid().cell(1, 0).state == CellState::Playing);
    CHECK(m.grid().cell(0, 0).state == CellState::Playing);
    const std::vector<Placement> p = bounce_scene(m.grid(), 0, 0, 4 * kBar);
    CHECK(p.size() == 2);
    CHECK(p[0].clip == 5 && p[1].clip == 99);
    return 0;
}

}  // namespace

int main() {
    if (test_bounce_scene_skips_empty_cells()) return 1;
    if (test_bounce_cell()) return 1;
    if (test_recorded_cell_bounces()) return 1;
    std::printf("launch_bounce: ok\n");
    return 0;
}
