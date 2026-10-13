// SPDX-License-Identifier: GPL-3.0-or-later
//
// --measure: what the shell costs, from inside the process.
//
// Read the numbers for what they are. Under a software rasteriser (the
// container, llvmpipe) they measure the CPU cost of drawing on the CPU,
// which ADR-0006 already says is not a supported configuration; they say
// nothing about a GPU. On the target they are the real figures, but the
// figure that matters -- whether the interface costs the audio callback
// anything -- needs the engine's xrun and callback-duration counters beside
// them, which only exist on the appliance.
#pragma once

#include <QString>
#include <atomic>
#include <cstdint>
#include <vector>

#include "shell_model.h"

class QQuickWindow;

namespace sonora::ui {

class FrameMeter {
public:
    explicit FrameMeter(std::size_t capacity);
    // Hooks the window's render-thread signals. Samples land in pre-sized
    // arrays; the render thread never allocates or locks for them.
    void attach(QQuickWindow* window);
    // Marks the start of the measured interval (after warm-up).
    void begin();
    // Stops sampling and returns a human-readable report.
    QString report(const TickStats& ticks, const QString& label, int meters,
                   int meter_hz, const QString& render_path);

private:
    struct Sample {
        int64_t render_ns;   // beforeRendering -> afterRendering, render thread
        int64_t swapped_ns;  // monotonic time of frameSwapped
    };
    std::vector<Sample> samples_;
    std::atomic<std::size_t> count_{0};
    std::atomic<bool> running_{false};
    int64_t render_start_ns_ = 0;  // render thread only
    int64_t begin_wall_ns_ = 0;
    int64_t begin_cpu_ns_ = 0;
};

}  // namespace sonora::ui
