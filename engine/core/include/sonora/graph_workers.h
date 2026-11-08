// SPDX-License-Identifier: GPL-3.0-or-later
//
// The help-first worker pool (ADR-0063 §3-§6): a GraphScheduler with N
// threads started when the session is created.
//
// Per run_level() the audio thread opens an epoch: every offered node is
// marked ready for it and a cursor {epoch, index} is published. Workers and
// the audio thread claim indices from the cursor; a claimer then moves the
// node from ready to running with a compare-exchange keyed on the epoch, so
// a claim that arrives after the level was settled never runs anything.
// Once the cursor is exhausted the audio thread polls the nodes workers hold,
// with a CPU pause, until they are done or the level's deadline passes. At
// the deadline a node still ready is taken back, a node still running is
// marked abandoned; both are dropped (GraphNodeWork::drop) and counted.
//
// An abandoned node's worker may still be inside run() when drop() is
// called, and may return from it blocks later: that late result is
// discarded (late_results() counts it) and the node is not offered again
// until its worker has returned; meanwhile it is dropped every block, which
// puts it down after kGraphDownAfterDrops. So the session's drop() must not
// write the buffer run() writes, and must not read it for that block.
//
// Audio thread: no allocation, lock, wait or syscall except the clock read
// while it spins for a worker (GraphClock, ADR-0063 Unverified 3).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sonora/graph_schedule.h"
#include "sonora/rt.h"

namespace sonora {

// The deadline's clock: the one AudioBlock::callback_ns is stamped on.
class GraphClock {
public:
    virtual uint64_t now_ns() const noexcept SONORA_RT = 0;

protected:
    ~GraphClock() = default;
};

// CLOCK_MONOTONIC (std::chrono::steady_clock), as the backends stamp it.
const GraphClock& monotonic_graph_clock() noexcept;

// Runs on each new worker thread before it takes any work: priority,
// affinity, memory lock. A non-null return is why it failed, and fails the
// pool's start.
class GraphWorkerSetup {
public:
    virtual const char* prepare(uint32_t worker) noexcept = 0;

protected:
    ~GraphWorkerSetup() = default;
};

struct GraphWorkerOptions {
    // nullptr: monotonic_graph_clock().
    const GraphClock* clock = nullptr;
    // nullptr: SCHED_FIFO at rt_priority when rt_priority > 0, else nothing.
    GraphWorkerSetup* setup = nullptr;
    // ADR-0063 §5: one below the audio thread. 0 keeps the inherited policy.
    int rt_priority = 0;
    // An idle worker spins this many pauses, then sleeps idle_sleep_ns per
    // poll: timer-paced, no wake gate (kGraphWakeEnabled is off).
    uint32_t idle_spins = 4096;
    uint32_t idle_sleep_ns = 50000;
};

// The futex wake gate of ADR-0063 §5 stays off until its cost is measured.
constexpr bool kGraphWakeEnabled = false;

class GraphWorkerPool : public GraphScheduler {
public:
    // Worker results that arrived after their node was dropped, from any
    // thread.
    virtual uint64_t late_results() const noexcept = 0;
};

struct GraphWorkersStarted {
    std::unique_ptr<GraphWorkerPool> pool;  // null when the start failed
    std::string error;                      // why, for the session's error
};

// Starts config.workers threads and returns once every one has run its
// setup. Fails loudly (ADR-0063 §6), with every thread already stopped:
// workers = 0 (that is the serial scheduler), max_nodes past what the cursor
// holds, a thread that cannot be created, or a setup that refuses.
// Control thread. The pool's destructor joins the workers, so a node's
// run() must return.
GraphWorkersStarted start_graph_workers(const GraphSchedulerConfig& config,
                                        const GraphWorkerOptions& options = {});

}  // namespace sonora
