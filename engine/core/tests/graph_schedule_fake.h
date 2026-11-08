// SPDX-License-Identifier: GPL-3.0-or-later
//
// A GraphScheduler for tests (ADR-0063): serial, plus a stall a test sets on
// a node. A stalled node is what a worker that claimed it and never finished
// looks like to the audio thread at the deadline: dropped, never run, and
// counted. Down nodes are skipped as the worker pool skips them. No thread
// and no clock, so a test of drops never waits on time.
#pragma once

#include <memory>

#include "sonora/graph_schedule.h"

namespace graph_test {

class FakeGraphScheduler final : public sonora::GraphScheduler {
public:
    // `workers` is only what workers() reports, so a session sees a
    // "parallel" scheduler; the fake runs everything on the caller.
    explicit FakeGraphScheduler(uint32_t max_nodes, uint32_t workers = 0)
        : book_(max_nodes),
          stalled_(std::make_unique<bool[]>(max_nodes + 1u)),
          workers_(workers) {}

    // Holds until cleared; a node index past max_nodes is ignored.
    void set_stalled(uint32_t node, bool stalled) {
        if (node < book_.capacity()) stalled_[node] = stalled;
    }

    const sonora::GraphDropBook& book() const { return book_; }

    uint32_t workers() const noexcept override { return workers_; }

    sonora::GraphLevelResult run_level(const sonora::GraphLevel& level,
                                       sonora::GraphNodeWork& work) noexcept SONORA_RT override {
        sonora::GraphLevelResult r;
        for (uint32_t i = 0; i < level.count; ++i) {
            const uint32_t n = level.nodes[i];
            if (book_.down(n)) {
                work.drop(n);
                book_.note_skipped(n);
                ++r.skipped;
            } else if (n < book_.capacity() && stalled_[n]) {
                work.drop(n);
                book_.note_dropped(n);
                ++r.dropped;
            } else {
                work.run(n);
                book_.note_ran(n);
                ++r.ran;
            }
        }
        return r;
    }

    void reset_drops() noexcept SONORA_RT override { book_.reset(); }

    sonora::GraphDropCounts drop_counts() const noexcept override { return book_.counts(); }

private:
    sonora::GraphDropBook book_;
    std::unique_ptr<bool[]> stalled_;
    const uint32_t workers_;
};

}  // namespace graph_test
