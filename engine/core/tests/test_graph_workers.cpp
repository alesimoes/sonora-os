// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0063's worker pool: equal to the serial scheduler bit for bit, a node a
// worker holds past the deadline dropped and counted, its late result
// discarded, eight drops put it down, and a pool that cannot start says so.
// Stalls are a worker held inside run() until the test lets go; the deadline
// is a test clock that moves one tick per read. No sleep, no wall clock.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

#include "sonora/graph_schedule.h"
#include "sonora/graph_workers.h"

namespace {

using namespace playback_test;
using sonora::GraphLevel;
using sonora::GraphLevelResult;

// A wait on another thread's progress. The bound only turns a broken pool
// into a failure instead of a hung test; it is never what a test relies on.
template <typename Pred>
bool wait_for(Pred pred) {
    for (uint64_t i = 0; i < (uint64_t{1} << 28); ++i) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return false;
}

// The same wait inside a node's run(): on the audio thread that is inside
// run_level(), on a worker inside worker_step(), both nonblocking, where
// RTSan rejects sched_yield, so it spins. The bound is
// wider because a spin iteration is much shorter than a yield.
template <typename Pred>
bool spin_for(Pred pred) noexcept {
    for (uint64_t i = 0; i < (uint64_t{1} << 32); ++i) {
        if (pred()) return true;
    }
    return false;
}

// ---- Bit-exactness ------------------------------------------------------

constexpr uint32_t kFrames = 256;
constexpr uint32_t kBlocks = 40;
// Three levels: 16 sources, 8 pairs of them, 4 pairs of pairs.
constexpr uint32_t kNodes = 28;
constexpr uint32_t kLevelFirst[] = {0, 16, 24, 28};
constexpr uint32_t kLevels = 3;

// Each node reads only lower levels and writes only its own row, as the
// session's nodes do; the mix is summed afterwards, in node order.
class LayeredWork final : public sonora::GraphNodeWork {
public:
    void run(uint32_t n) noexcept override {
        if (std::this_thread::get_id() != caller) on_workers.fetch_add(1, std::memory_order_relaxed);
        float* out = rows[n];
        if (n < 16) {
            for (uint32_t i = 0; i < kFrames; ++i) {
                const float t = static_cast<float>(block * kFrames + i) * (0.001f + 0.0003f * n);
                out[i] = std::sin(t) * (0.5f + 0.01f * static_cast<float>(n));
            }
        } else if (n < 24) {
            const float* a = rows[2 * (n - 16)];
            const float* b = rows[2 * (n - 16) + 1];
            for (uint32_t i = 0; i < kFrames; ++i) out[i] = std::tanh(a[i] * 0.7f + b[i] * 0.31f);
        } else {
            const float* a = rows[16 + 2 * (n - 24)];
            const float* b = rows[16 + 2 * (n - 24) + 1];
            for (uint32_t i = 0; i < kFrames; ++i) out[i] = a[i] * 0.83f - b[i] * 0.29f;
        }
    }
    void drop(uint32_t n) noexcept override {
        ++drops;
        std::memset(rows[n], 0, sizeof(rows[n]));
    }

    float rows[kNodes][kFrames] = {};
    uint32_t block = 0;
    uint32_t drops = 0;
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<uint64_t> on_workers{0};
};

// Runs kBlocks through `s` and leaves every block's mix in `mix`.
int render(sonora::GraphScheduler& s, LayeredWork& work, float* mix) {
    uint32_t order[kNodes];
    for (uint32_t i = 0; i < kNodes; ++i) order[i] = i;
    for (uint32_t b = 0; b < kBlocks; ++b) {
        work.block = b;
        t_counting = true;
        for (uint32_t l = 0; l < kLevels; ++l) {
            GraphLevel level;
            level.nodes = order + kLevelFirst[l];
            level.count = kLevelFirst[l + 1] - kLevelFirst[l];
            level.deadline_ns = UINT64_MAX;
            const GraphLevelResult r = s.run_level(level, work);
            CHECK(r.ran == level.count && r.dropped == 0 && r.skipped == 0);
        }
        t_counting = false;
        float* out = mix + b * kFrames;
        for (uint32_t i = 0; i < kFrames; ++i) out[i] = 0.0f;
        for (uint32_t n = 0; n < kNodes; ++n) {
            for (uint32_t i = 0; i < kFrames; ++i) out[i] += work.rows[n][i];
        }
    }
    CHECK(work.drops == 0);
    return 0;
}

int test_bit_exact_with_serial() {
    static float serial_mix[kBlocks * kFrames];
    static float pool_mix[kBlocks * kFrames];
    {
        auto serial = sonora::make_serial_graph_scheduler();
        auto work = std::make_unique<LayeredWork>();
        if (render(*serial, *work, serial_mix)) return 1;
    }
    for (uint32_t n = 1; n <= 4; ++n) {
        sonora::GraphSchedulerConfig config;
        config.workers = n;
        config.max_nodes = kNodes;
        sonora::GraphWorkerOptions options;
        options.idle_sleep_ns = 1000;
        sonora::GraphWorkersStarted started = sonora::start_graph_workers(config, options);
        CHECK(started.pool != nullptr);
        CHECK(started.error.empty());
        CHECK(started.pool->workers() == n);
        auto work = std::make_unique<LayeredWork>();
        reset_heap_counts();
        if (render(*started.pool, *work, pool_mix)) return 1;
        CHECK(heap_untouched());
        CHECK(std::memcmp(serial_mix, pool_mix, sizeof(serial_mix)) == 0);
        CHECK(started.pool->drop_counts().dropped == 0);
        CHECK(started.pool->drop_counts().skipped == 0);
        CHECK(started.pool->late_results() == 0);
        std::printf("graph_workers: N=%u bit-exact, %llu of %u node runs on workers\n", n,
                    static_cast<unsigned long long>(work->on_workers.load()), kBlocks * kNodes);
    }
    return 0;
}

// ---- Stalls -------------------------------------------------------------

// One tick per read, so the audio thread's spin reaches any deadline after
// a known amount of polling, whatever the machine.
class TickClock final : public sonora::GraphClock {
public:
    uint64_t now_ns() const noexcept override {
        return ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    mutable std::atomic<uint64_t> ticks{0};
};

// Two nodes, one worker. In `handshake` mode the audio thread's run() waits
// until the worker has entered run() on the other node, so the worker is
// known to hold one of them; the worker's run() then holds until `release`.
class StallWork final : public sonora::GraphNodeWork {
public:
    explicit StallWork(const TickClock& clock) : clock_(clock) {}

    void run(uint32_t n) noexcept override {
        runs[n].fetch_add(1, std::memory_order_relaxed);
        if (std::this_thread::get_id() == caller) {
            if (handshake) handshake_ok = spin_for([&] { return worker_node.load() >= 0; });
            return;
        }
        if (!hold) return;
        worker_node.store(static_cast<int>(n));
        spin_for([&] { return release.load(); });
        worker_returned.store(true);
    }
    void drop(uint32_t n) noexcept override {
        drops[n] += 1;
        drop_tick = clock_.ticks.load();
    }

    std::thread::id caller = std::this_thread::get_id();
    bool handshake = true;
    bool handshake_ok = true;
    std::atomic<bool> hold{true};
    std::atomic<bool> release{false};
    std::atomic<int> worker_node{-1};
    std::atomic<bool> worker_returned{false};
    std::atomic<uint32_t> runs[2] = {};
    uint32_t drops[2] = {};
    uint64_t drop_tick = 0;

private:
    const TickClock& clock_;
};

struct StallRig {
    TickClock clock;
    std::unique_ptr<StallWork> work;
    std::unique_ptr<sonora::GraphWorkerPool> pool;
    const uint32_t nodes[2] = {0, 1};

    // A failed CHECK must not leave the pool joining a held worker forever.
    ~StallRig() {
        if (work) work->release.store(true);
    }

    bool start() {
        work = std::make_unique<StallWork>(clock);
        sonora::GraphSchedulerConfig config;
        config.workers = 1;
        config.max_nodes = 2;
        sonora::GraphWorkerOptions options;
        options.clock = &clock;
        options.idle_sleep_ns = 1000;
        pool = sonora::start_graph_workers(config, options).pool;
        return pool != nullptr;
    }
    GraphLevelResult block(uint64_t deadline) {
        GraphLevel level;
        level.nodes = nodes;
        level.count = 2;
        level.deadline_ns = deadline;
        t_counting = true;
        const GraphLevelResult r = pool->run_level(level, *work);
        t_counting = false;
        return r;
    }
    // Lets a held worker return and waits until the pool has taken its
    // late result back, so the next block may offer the node again.
    bool let_go() {
        work->release.store(true);
        return wait_for([&] { return pool->late_results() == 1; });
    }
};

int test_stalled_node_is_dropped_at_the_deadline() {
    StallRig rig;
    CHECK(rig.start());
    constexpr uint64_t kDeadline = 1000;
    reset_heap_counts();
    const GraphLevelResult r = rig.block(kDeadline);
    CHECK(rig.work->handshake_ok);
    const int held = rig.work->worker_node.load();
    CHECK(held == 0 || held == 1);
    const int other = 1 - held;
    // The audio thread ran its node, spun to the deadline, dropped the held
    // one, and returned while the worker is still inside run().
    CHECK(r.ran == 1 && r.dropped == 1 && r.skipped == 0);
    CHECK(rig.work->drops[held] == 1 && rig.work->drops[other] == 0);
    CHECK(rig.work->drop_tick >= kDeadline);
    CHECK(!rig.work->worker_returned.load());
    CHECK(rig.pool->drop_counts().dropped == 1);
    CHECK(heap_untouched());
    CHECK(rig.let_go());
    return 0;
}

int test_late_result_is_ignored() {
    StallRig rig;
    CHECK(rig.start());
    rig.block(1000);
    const int held = rig.work->worker_node.load();
    CHECK(held == 0 || held == 1);
    const int other = 1 - held;
    CHECK(rig.work->drops[held] == 1);

    // Next block, worker still out: the node is not offered, so the block
    // neither runs it twice at once nor waits for it, deadline or not.
    rig.work->handshake = false;
    const uint64_t ticks_before = rig.clock.ticks.load();
    GraphLevelResult r = rig.block(UINT64_MAX);
    CHECK(r.ran == 1 && r.dropped == 1);
    CHECK(rig.work->drops[held] == 2);
    CHECK(rig.work->runs[held].load() == 1);
    CHECK(rig.clock.ticks.load() == ticks_before);

    // The worker comes back: its result is counted late, not as a run, and
    // the node is offered again from the next block on.
    CHECK(rig.let_go());
    CHECK(rig.pool->drop_counts().dropped == 2);
    rig.work->hold.store(false);
    r = rig.block(UINT64_MAX);
    CHECK(r.ran == 2 && r.dropped == 0 && r.skipped == 0);
    CHECK(rig.work->runs[held].load() == 2);
    CHECK(rig.work->runs[other].load() == 3);
    CHECK(rig.work->drops[held] == 2 && rig.work->drops[other] == 0);
    CHECK(rig.pool->late_results() == 1);
    return 0;
}

int test_eight_drops_silence_the_node_until_reset() {
    StallRig rig;
    CHECK(rig.start());
    rig.block(1000);
    const int held = rig.work->worker_node.load();
    CHECK(held == 0 || held == 1);
    rig.work->handshake = false;
    for (uint32_t b = 1; b < sonora::kGraphDownAfterDrops; ++b) {
        const GraphLevelResult r = rig.block(UINT64_MAX);
        CHECK(r.dropped == 1 && r.skipped == 0);
    }
    CHECK(rig.pool->drop_counts().dropped == sonora::kGraphDownAfterDrops);

    // Down: skipped and counted, even after its worker is back.
    GraphLevelResult r = rig.block(UINT64_MAX);
    CHECK(r.ran == 1 && r.dropped == 0 && r.skipped == 1);
    CHECK(rig.let_go());
    rig.work->hold.store(false);
    r = rig.block(UINT64_MAX);
    CHECK(r.skipped == 1);
    CHECK(rig.pool->drop_counts().skipped == 2);
    CHECK(rig.work->runs[held].load() == 1);

    // A plan swap gives it its chance again.
    rig.pool->reset_drops();
    r = rig.block(UINT64_MAX);
    CHECK(r.ran == 2 && r.dropped == 0 && r.skipped == 0);
    CHECK(rig.work->runs[held].load() == 2);
    return 0;
}

// ---- Start --------------------------------------------------------------

class RefusingSetup final : public sonora::GraphWorkerSetup {
public:
    const char* prepare(uint32_t worker) noexcept override {
        prepared.fetch_add(1);
        return worker == 2 ? "could not set its SCHED_FIFO priority" : nullptr;
    }
    std::atomic<uint32_t> prepared{0};
};

int test_start_failure_returns_an_error() {
    sonora::GraphSchedulerConfig config;
    config.workers = 3;
    config.max_nodes = 8;
    RefusingSetup setup;
    sonora::GraphWorkerOptions options;
    options.setup = &setup;
    sonora::GraphWorkersStarted started = sonora::start_graph_workers(config, options);
    CHECK(started.pool == nullptr);
    CHECK(started.error.find("graph worker 2") != std::string::npos);
    CHECK(started.error.find("SCHED_FIFO") != std::string::npos);
    CHECK(setup.prepared.load() == 3);

    config.workers = 0;
    started = sonora::start_graph_workers(config);
    CHECK(started.pool == nullptr && !started.error.empty());

    config.workers = 1;
    config.max_nodes = 1u << 24;
    started = sonora::start_graph_workers(config);
    CHECK(started.pool == nullptr && !started.error.empty());
    return 0;
}

}  // namespace

int main() {
    if (test_bit_exact_with_serial()) return 1;
    if (test_stalled_node_is_dropped_at_the_deadline()) return 1;
    if (test_late_result_is_ignored()) return 1;
    if (test_eight_drops_silence_the_node_until_reset()) return 1;
    if (test_start_failure_returns_an_error()) return 1;
    std::printf("graph_workers: ok (heap counter: %s)\n", heap_counter_scope());
    return 0;
}
