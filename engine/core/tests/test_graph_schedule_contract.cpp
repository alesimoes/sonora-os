// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0063's contract: the plan's level ranges, the serial scheduler as the
// walk it replaces, and the drop rules every scheduler shares, exercised
// through the fake's injected stall (no thread, no clock).
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cmath>
#include <cstdint>

#include "graph_schedule_fake.h"
#include "sonora/graph_schedule.h"
#include "sonora/playback_plan.h"

namespace {

using namespace playback_test;
using sonora::GraphLevel;
using sonora::GraphLevelResult;

// What a scheduler asked of the session, in order: +n ran node n, -(n+1)
// dropped it. Fixed storage, so recording allocates nothing.
class RecordingWork final : public sonora::GraphNodeWork {
public:
    void run(uint32_t node) noexcept override { push(static_cast<int64_t>(node)); }
    void drop(uint32_t node) noexcept override { push(-static_cast<int64_t>(node) - 1); }

    void clear() { count = 0; }
    bool ran(uint32_t node) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (calls[i] == static_cast<int64_t>(node)) return true;
        }
        return false;
    }
    bool dropped(uint32_t node) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (calls[i] == -static_cast<int64_t>(node) - 1) return true;
        }
        return false;
    }

    int64_t calls[64] = {};
    uint32_t count = 0;

private:
    void push(int64_t v) {
        if (count < 64) calls[count++] = v;
    }
};

int test_plan_levels() {
    sonora::PlanSizes sizes;
    sizes.tracks = 2;
    sizes.nodes = 4;
    sizes.edges = 3;
    sonora::PlaybackPlan* plan = sonora::create_plan(sizes);
    CHECK(plan != nullptr);
    // Every plan carries room for node_count + 1 level bounds, unset.
    CHECK(plan->level_first != nullptr);
    CHECK(plan->level_count == 0);
    for (uint32_t i = 0; i <= sizes.nodes; ++i) CHECK(plan->level_first[i] == 0);
    sonora::layout_monitor_graph(*plan);
    for (uint32_t i = 0; i < plan->node_count; ++i) CHECK(plan->nodes[i].level == 0);
    CHECK(plan->nodes[2].kind == sonora::NodeKind::BusSum);
    CHECK(plan->nodes[3].kind == sonora::NodeKind::HardwareOut);

    // No level pass: one level, the whole serial walk.
    uint32_t first = 99;
    uint32_t count = 99;
    CHECK(sonora::plan_level_count(*plan) == 1);
    sonora::plan_level_range(*plan, 0, first, count);
    CHECK(first == 0 && count == 4);

    // The monitor graph's levels: the tracks, the bus, the output. The last
    // bound lands on the array's last entry.
    plan->level_count = 3;
    const uint32_t bounds[] = {0, 2, 3, 4};
    for (uint32_t i = 0; i < 4; ++i) plan->level_first[i] = bounds[i];
    CHECK(sonora::plan_level_count(*plan) == 3);
    sonora::plan_level_range(*plan, 0, first, count);
    CHECK(first == 0 && count == 2);
    sonora::plan_level_range(*plan, 1, first, count);
    CHECK(first == 2 && count == 1);
    sonora::plan_level_range(*plan, 2, first, count);
    CHECK(first == 3 && count == 1);
    sonora::destroy_plan(plan);

    sonora::PlaybackPlan* empty = sonora::create_plan(sonora::PlanSizes{});
    CHECK(empty != nullptr);
    CHECK(empty->level_first != nullptr);
    CHECK(sonora::plan_level_count(*empty) == 0);
    sonora::destroy_plan(empty);
    return 0;
}

int test_serial_runs_every_node_in_order() {
    const auto serial = sonora::make_serial_graph_scheduler();
    CHECK(serial != nullptr);
    CHECK(serial->workers() == 0);
    RecordingWork work;
    const uint32_t nodes[] = {4, 1, 3};
    GraphLevel level;
    level.nodes = nodes;
    level.count = 3;
    level.deadline_ns = 0;   // long past: serial never drops for it

    reset_heap_counts();
    t_counting = true;
    const GraphLevelResult r = serial->run_level(level, work);
    serial->reset_drops();
    t_counting = false;
    CHECK(heap_untouched());

    CHECK(r.ran == 3 && r.dropped == 0 && r.skipped == 0);
    CHECK(work.count == 3);
    CHECK(work.calls[0] == 4 && work.calls[1] == 1 && work.calls[2] == 3);
    CHECK(serial->drop_counts().dropped == 0 && serial->drop_counts().skipped == 0);

    work.clear();
    const GraphLevelResult none = serial->run_level(GraphLevel{}, work);
    CHECK(none.ran == 0 && work.count == 0);
    return 0;
}

int test_stalled_node_is_dropped_not_run() {
    graph_test::FakeGraphScheduler fake(8, 3);
    CHECK(fake.workers() == 3);
    fake.set_stalled(1, true);
    RecordingWork work;
    const uint32_t nodes[] = {0, 1, 2};
    GraphLevel level;
    level.nodes = nodes;
    level.count = 3;

    reset_heap_counts();
    t_counting = true;
    const GraphLevelResult r = fake.run_level(level, work);
    t_counting = false;
    CHECK(heap_untouched());

    CHECK(r.ran == 2 && r.dropped == 1 && r.skipped == 0);
    CHECK(work.ran(0) && work.ran(2));
    CHECK(work.dropped(1) && !work.ran(1));
    CHECK(work.count == 3);   // once each, never run and dropped
    CHECK(fake.drop_counts().dropped == 1);
    CHECK(fake.book().consecutive_drops(1) == 1);
    return 0;
}

int test_eight_drops_put_a_node_down_until_reset() {
    graph_test::FakeGraphScheduler fake(4);
    RecordingWork work;
    const uint32_t nodes[] = {2};
    GraphLevel level;
    level.nodes = nodes;
    level.count = 1;

    fake.set_stalled(2, true);
    for (uint32_t b = 0; b < sonora::kGraphDownAfterDrops; ++b) {
        CHECK(!fake.book().down(2));
        fake.run_level(level, work);
    }
    CHECK(fake.book().down(2));
    CHECK(fake.drop_counts().dropped == sonora::kGraphDownAfterDrops);

    // Down: no longer offered to run even with the stall gone.
    fake.set_stalled(2, false);
    work.clear();
    const GraphLevelResult r = fake.run_level(level, work);
    CHECK(r.ran == 0 && r.dropped == 0 && r.skipped == 1);
    CHECK(work.dropped(2) && !work.ran(2));
    CHECK(fake.drop_counts().skipped == 1);
    CHECK(fake.drop_counts().dropped == sonora::kGraphDownAfterDrops);

    // A plan swap gives it its chance again; totals stay.
    fake.reset_drops();
    CHECK(!fake.book().down(2));
    work.clear();
    const GraphLevelResult back = fake.run_level(level, work);
    CHECK(back.ran == 1 && work.ran(2));
    CHECK(fake.drop_counts().skipped == 1);
    return 0;
}

int test_a_run_breaks_the_streak() {
    graph_test::FakeGraphScheduler fake(4);
    RecordingWork work;
    const uint32_t nodes[] = {0};
    GraphLevel level;
    level.nodes = nodes;
    level.count = 1;
    for (int round = 0; round < 2; ++round) {
        fake.set_stalled(0, true);
        for (uint32_t b = 0; b + 1 < sonora::kGraphDownAfterDrops; ++b) fake.run_level(level, work);
        fake.set_stalled(0, false);
        fake.run_level(level, work);
        CHECK(fake.book().consecutive_drops(0) == 0);
    }
    CHECK(!fake.book().down(0));
    CHECK(fake.drop_counts().dropped == 2 * (sonora::kGraphDownAfterDrops - 1));
    return 0;
}

int test_deadline() {
    CHECK(sonora::graph_deadline_ns(1000, 1000, 0.7f) == 1700);
    CHECK(sonora::graph_deadline_ns(1000, 1000, 2.0f) == 2000);
    CHECK(sonora::graph_deadline_ns(1000, 1000, 0.0f) == 1000);
    CHECK(sonora::graph_deadline_ns(1000, 1000, std::nanf("")) == 1000);
    // 128 frames at 48 kHz: 70 % of 2 666 666 ns.
    CHECK(sonora::graph_deadline_ns(0, 2666666, sonora::kGraphDeadlineFraction) == 1866666);
    return 0;
}

}  // namespace

int main() {
    if (test_plan_levels()) return 1;
    if (test_serial_runs_every_node_in_order()) return 1;
    if (test_stalled_node_is_dropped_not_run()) return 1;
    if (test_eight_drops_put_a_node_down_until_reset()) return 1;
    if (test_a_run_breaks_the_streak()) return 1;
    if (test_deadline()) return 1;
    std::printf("graph_schedule_contract: ok (heap counter: %s)\n", heap_counter_scope());
    return 0;
}
