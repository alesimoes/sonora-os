// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/graph_schedule.h"

namespace sonora {
namespace {

// No deadline and no book: a node that runs on the audio thread is never
// dropped, which is exactly the serial walk before ADR-0063.
class SerialGraphScheduler final : public GraphScheduler {
public:
    uint32_t workers() const noexcept override { return 0; }

    GraphLevelResult run_level(const GraphLevel& level,
                               GraphNodeWork& work) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < level.count; ++i) work.run(level.nodes[i]);
        GraphLevelResult r;
        r.ran = level.count;
        return r;
    }

    void reset_drops() noexcept SONORA_RT override {}

    GraphDropCounts drop_counts() const noexcept override { return {}; }
};

}  // namespace

std::unique_ptr<GraphScheduler> make_serial_graph_scheduler() {
    return std::make_unique<SerialGraphScheduler>();
}

}  // namespace sonora
