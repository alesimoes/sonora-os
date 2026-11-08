// SPDX-License-Identifier: GPL-3.0-or-later
//
// How Session::process() runs one level of the plan's nodes (ADR-0063).
//
// The session walks the plan level by level (plan_level_count(),
// plan_level_range()). For each level it gathers the nodes that may run on a
// worker (ADR-0063 §7: parallel-safe kinds) into an index array it sized at
// creation, hands them to a GraphScheduler, and runs the rest itself, in plan
// order, between levels. Every node of a GraphLevel is run once by
// GraphNodeWork::run() or dropped once by GraphNodeWork::drop(), never both,
// and run_level() returns only when that is true of all of them.
//
// The serial scheduler (make_serial_graph_scheduler) runs them in order on
// the calling thread and never drops: today's output, workers = 0. The
// worker pool (graph_workers.h) helps first, spins only to the level's
// deadline and drops what is still out at it; a drop is ramped silence and
// counted (ADR-0063 §4). Tests inject stalls through
// engine/core/tests/graph_schedule_fake.h, never through time.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

#include "sonora/rt.h"

namespace sonora {

// The share of the period a level set may take before its stragglers are
// dropped (ADR-0063 §4; Unverified 2 settles the figure).
constexpr float kGraphDeadlineFraction = 0.7f;
// A node dropped this many blocks in a row is down: skipped, and counted,
// until the next plan (ADR-0063 §4, ADR-0012 §3's rule).
constexpr uint32_t kGraphDownAfterDrops = 8;

// The deadline of a callback that starts at `callback_ns` (AudioBlock's
// stamp) with a period of `period_ns`. On the same clock as the stamp.
inline uint64_t graph_deadline_ns(uint64_t callback_ns, uint64_t period_ns,
                                  float fraction) noexcept SONORA_RT {
    if (!(fraction > 0.0f)) return callback_ns;
    if (fraction > 1.0f) fraction = 1.0f;
    // Rounded: 0.7f is a hair under 0.7, and truncation would lose a ns.
    return callback_ns + static_cast<uint64_t>(static_cast<double>(period_ns) * fraction + 0.5);
}

// One node's work, by plan node index, implemented by the session.
class GraphNodeWork {
public:
    // Runs the node into its own scratch buffer, reading only outputs of
    // lower levels. A scheduler with workers calls it on a worker thread,
    // so it must touch nothing another node of the level writes.
    virtual void run(uint32_t node) noexcept SONORA_RT = 0;
    // The node missed its deadline or is down: its scratch gets silence,
    // ramped from the last block it ran. Always on the calling (audio)
    // thread, and only once no worker can still write that scratch for this
    // block.
    virtual void drop(uint32_t node) noexcept SONORA_RT = 0;

protected:
    // Owned by the session; never deleted through this type.
    ~GraphNodeWork() = default;
};

// The nodes one run_level() call runs, as plan node indices.
struct GraphLevel {
    const uint32_t* nodes = nullptr;
    uint32_t count = 0;
    // graph_deadline_ns() of this callback. The serial scheduler ignores it.
    uint64_t deadline_ns = 0;
};

struct GraphLevelResult {
    uint32_t ran = 0;
    uint32_t dropped = 0;   // missed the deadline this block
    uint32_t skipped = 0;   // down: not offered to run at all
};

// What a session reads back, from any thread.
struct GraphDropCounts {
    uint64_t dropped = 0;
    uint64_t skipped = 0;
};

// The audio thread's book of drops, sized for a session's most nodes at
// creation. Totals are atomics another thread may read; the per-node
// history belongs to the audio thread alone.
class GraphDropBook {
public:
    // Zeroed here, so the audio thread faults in no page of it.
    explicit GraphDropBook(uint32_t max_nodes)
        : capacity_(max_nodes), consecutive_(std::make_unique<uint8_t[]>(max_nodes + 1u)) {}

    uint32_t capacity() const noexcept { return capacity_; }

    bool down(uint32_t node) const noexcept SONORA_RT {
        return node < capacity_ && consecutive_[node] >= kGraphDownAfterDrops;
    }
    uint32_t consecutive_drops(uint32_t node) const noexcept SONORA_RT {
        return node < capacity_ ? consecutive_[node] : 0;
    }
    void note_ran(uint32_t node) noexcept SONORA_RT {
        if (node < capacity_) consecutive_[node] = 0;
    }
    void note_dropped(uint32_t node) noexcept SONORA_RT {
        if (node < capacity_ && consecutive_[node] < kGraphDownAfterDrops) ++consecutive_[node];
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_skipped(uint32_t) noexcept SONORA_RT {
        skipped_.fetch_add(1, std::memory_order_relaxed);
    }
    // A new plan: node indices name other nodes now, so every history is
    // forgotten and a down node gets its chance again. Totals stay.
    void reset() noexcept SONORA_RT {
        std::memset(consecutive_.get(), 0, capacity_);
    }

    GraphDropCounts counts() const noexcept {
        return {dropped_.load(std::memory_order_relaxed), skipped_.load(std::memory_order_relaxed)};
    }

private:
    const uint32_t capacity_;
    // Saturates at kGraphDownAfterDrops, so a byte holds it.
    std::unique_ptr<uint8_t[]> consecutive_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> skipped_{0};
};

// What the session's worker setting asks for (ADR-0063 §5, §6), fixed at
// session creation.
struct GraphSchedulerConfig {
    uint32_t workers = 0;           // 0: the serial scheduler
    uint32_t max_nodes = 0;         // the most nodes any plan of the session has
    float deadline_fraction = kGraphDeadlineFraction;
};

class GraphScheduler {
public:
    virtual ~GraphScheduler() = default;

    // Threads besides the caller that run nodes; 0 for the serial one.
    virtual uint32_t workers() const noexcept = 0;
    // Runs or drops every node of `level` once (see the file comment).
    // Audio thread: no allocation, lock, wait or syscall; a scheduler with
    // workers may spin with a CPU pause up to level.deadline_ns, no longer.
    virtual GraphLevelResult run_level(const GraphLevel& level,
                                       GraphNodeWork& work) noexcept SONORA_RT = 0;
    // At a plan swap, on the audio thread, before the new plan's first
    // level: per-node drop history forgotten (GraphDropBook::reset).
    virtual void reset_drops() noexcept SONORA_RT = 0;
    // Totals since creation, from any thread.
    virtual GraphDropCounts drop_counts() const noexcept = 0;
};

// Runs every node in order on the calling thread and never drops: the
// default, and the reference the worker pool must equal bit for bit.
std::unique_ptr<GraphScheduler> make_serial_graph_scheduler();

}  // namespace sonora
