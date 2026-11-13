// LaunchClock (ADR-0164): the launch clock free-runs while the transport is
// stopped, aligns the bar grid to the transport when it rolls, and splits a
// block at a loop wrap so a pending launch lands on the new grid's boundary.
#include "check.h"
#include "sonora/launch/launch_clock.h"
#include "sonora/launch/state_machine_launcher.h"

#include <vector>

using namespace sonora::launch;

namespace {

// 120 BPM 4/4 at 48 kHz.
constexpr std::int64_t kBar = 96000;

struct Hit {
    Transition::Kind kind;
    std::int64_t clock;      // launch-clock frame
    std::int64_t position;   // transport frame at that clock frame (rolling only)
    std::uint32_t offset;
};

StateMachineLauncher make() {
    LauncherConfig c;
    c.tracks = 1;
    c.scenes = 2;
    c.quantize = Quantize::Bar;
    return StateMachineLauncher(c);
}

// The callback's transport: rolls (or not) and loops like a real one would,
// one block at a time.
struct FakeTransport {
    BlockTransport t;
    void advance(std::uint32_t frames) {
        if (!t.rolling) return;
        t.position += frames;
        if (t.looping && t.position >= t.loop_end)
            t.position = t.loop_start + (t.position - t.loop_end);
    }
};

// Processes blocks until the clock reaches `until`, recording transitions.
void run(LaunchClock& clock, FakeTransport& tp, std::int64_t until, std::uint32_t block,
         std::vector<Hit>& out) {
    while (clock.now() < until) {
        const std::int64_t start = clock.now();
        const std::int64_t position = tp.t.position;
        const TransitionList list = clock.process(tp.t, block);
        for (std::uint32_t i = 0; i < list.count; ++i) {
            std::int64_t pos = position + list[i].offset;
            if (tp.t.looping && pos >= tp.t.loop_end) pos = tp.t.loop_start + (pos - tp.t.loop_end);
            out.push_back(Hit{list[i].kind, start + list[i].offset, pos, list[i].offset});
        }
        tp.advance(block);
    }
}

int test_stopped_transport_free_runs() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;  // stopped
    l.assign_clip(0, 0, 5);
    std::vector<Hit> hits;
    run(clock, tp, 1000, 500, hits);
    CHECK(clock.now() == 1000);  // the clock runs while stopped
    l.trigger_cell(0, 0);
    run(clock, tp, 3 * kBar, 500, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0].kind == Transition::Kind::Start);
    CHECK(hits[0].clock == kBar);  // the free-running grid's next bar line
    CHECK(clock.anchor() == 0);
    CHECK(tp.t.position == 0);
    return 0;
}

int test_tempo_change_while_stopped_resnaps() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;
    l.assign_clip(0, 0, 5);
    std::vector<Hit> hits;
    run(clock, tp, 1000, 500, hits);
    l.trigger_cell(0, 0);  // lands on 96000 at 120 BPM
    run(clock, tp, 2000, 500, hits);
    tp.t.tempo_bpm = 60.0;  // a bar is now 192000 frames
    run(clock, tp, 3 * kBar, 500, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0].clock == 2 * kBar);
    return 0;
}

int test_start_rolling_aligns_pending_to_transport_bar() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;
    l.assign_clip(0, 0, 5);
    std::vector<Hit> hits;
    run(clock, tp, 480, 480, hits);
    l.trigger_cell(0, 0);  // queued on the free-running bar at clock 96000
    run(clock, tp, 48000, 480, hits);
    CHECK(hits.empty());
    // Play from a quarter into bar 1: the next transport bar line is 72000
    // frames away, not where the stopped grid had it.
    tp.t.rolling = true;
    tp.t.position = 24000;
    run(clock, tp, 4 * kBar, 480, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0].clock == 48000 + 72000);
    CHECK(hits[0].position == kBar);
    CHECK(clock.anchor() == 24000);
    return 0;
}

int test_play_from_bar_starts_with_the_transport() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;
    l.assign_clip(0, 0, 5);
    std::vector<Hit> hits;
    run(clock, tp, 4800, 480, hits);
    l.trigger_cell(0, 0);
    run(clock, tp, 9600, 480, hits);
    tp.t.rolling = true;  // from bar 1
    run(clock, tp, 2 * kBar, 480, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0].clock == 9600 && hits[0].position == 0 && hits[0].offset == 0);
    return 0;
}

int test_loop_wrap_mid_block_resnaps() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;
    tp.t.rolling = true;
    tp.t.looping = true;
    tp.t.loop_end = kBar + kBar / 2;  // 1.5 bars: the wrap moves the grid
    l.assign_clip(0, 0, 5);
    std::vector<Hit> hits;
    run(clock, tp, 120000, 700, hits);
    l.trigger_cell(0, 0);  // bar 2 at transport 192000 is never reached
    run(clock, tp, 400000, 700, hits);
    CHECK(!hits.empty());
    // It lands on the wrap: transport bar 1 of the next pass, mid-block.
    CHECK(hits[0].clock == 144000);
    CHECK(hits[0].position == 0);
    CHECK(hits[0].offset == 144000 % 700);
    CHECK(hits[0].offset != 0);
    CHECK(hits.size() == 1);
    return 0;
}

int test_loop_wrap_whole_bars_keeps_boundary() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;
    tp.t.rolling = true;
    tp.t.looping = true;
    tp.t.loop_start = kBar;
    tp.t.loop_end = 3 * kBar;  // two whole bars: the grid is unchanged
    tp.t.position = kBar;
    l.assign_clip(0, 1, 6);
    std::vector<Hit> hits;
    run(clock, tp, 150000, 700, hits);
    l.trigger_cell(0, 1);  // next bar is the loop end, transport 288000
    run(clock, tp, 300000, 700, hits);
    CHECK(hits.size() == 1);
    // The wrap lands mid-block at clock 192000, on transport bar 2.
    CHECK(hits[0].clock == 192000);
    CHECK(hits[0].offset == 200);
    CHECK(hits[0].position == kBar);
    return 0;
}

int test_wrap_exactly_on_block_edge() {
    StateMachineLauncher l = make();
    LaunchClock clock(l);
    FakeTransport tp;
    tp.t.rolling = true;
    tp.t.looping = true;
    tp.t.loop_end = 3 * kBar / 2;  // 144000 = 300 x 480
    l.assign_clip(0, 0, 5);
    std::vector<Hit> hits;
    run(clock, tp, 120000, 480, hits);
    l.trigger_cell(0, 0);
    run(clock, tp, 200000, 480, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0].clock == 144000 && hits[0].offset == 0 && hits[0].position == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_stopped_transport_free_runs()) return 1;
    if (test_tempo_change_while_stopped_resnaps()) return 1;
    if (test_start_rolling_aligns_pending_to_transport_bar()) return 1;
    if (test_play_from_bar_starts_with_the_transport()) return 1;
    if (test_loop_wrap_mid_block_resnaps()) return 1;
    if (test_loop_wrap_whole_bars_keeps_boundary()) return 1;
    if (test_wrap_exactly_on_block_edge()) return 1;
    std::printf("launch_clock: ok\n");
    return 0;
}
