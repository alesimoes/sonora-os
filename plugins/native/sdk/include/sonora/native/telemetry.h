// SPDX-License-Identifier: GPL-3.0-or-later
//
// The publish helper for a native processor's telemetry (ADR-0057): gain
// reduction, levels and a response curve, written from process() into the
// telemetry region's block for this insert, for the engine's control thread
// to read and an editor to draw.
//
// The processor never branches on whether anyone listens. Until a host binds
// the shared block, writes land in a private one that nothing reads, so the
// same process() runs in the plugin host, the measurement harness and the
// unit tests. Everything here is wait-free, allocation-free and safe on the
// audio thread; bind() is not (a host calls it once, before audio).
//
//     tel_.begin();
//     tel_.meter(0, gain_reduction_db);
//     tel_.meter(1, input_db);
//     tel_.curve(0, response_.data(), 128);   // only when it changed
//     tel_.end();
//
// Publish once per block at most: a frame costs a few dozen relaxed stores.
// The engine reads it at kTelemetryMaxHz, whatever the block rate.
#pragma once

#include <cstdint>

#include "sonora/plugin/telemetry.h"
#include "sonora/rt.h"

namespace sonora::native {

class Telemetry {
public:
    Telemetry() noexcept : block_(&local_) {}
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    // Not real-time. nullptr goes back to the private block.
    void bind(plugin::TelemetryBlock* shared) noexcept { block_ = shared != nullptr ? shared : &local_; }
    bool bound() const noexcept { return block_ != &local_; }

    // One frame: begin(), any meter() and curve() calls, end(). A reader
    // never sees a frame that is half of one write and half of the next.
    void begin() noexcept SONORA_RT { block_->begin_write(); }
    void end() noexcept SONORA_RT { block_->end_write(); }
    // `slot` is the PluginMeterInfo::slot the descriptor declares. A value
    // that is not finite is published as 0 and counted in rejected().
    void meter(std::uint32_t slot, float value) noexcept SONORA_RT {
        block_->set_meter(slot, value);
    }
    // `points` samples of PluginCurveInfo `index`, at most kMaxCurvePoints.
    void curve(std::uint32_t index, const float* points, std::uint32_t count) noexcept SONORA_RT {
        block_->set_curve(index, points, count);
    }

    // How many non-finite values were replaced since the block was made.
    std::uint32_t rejected() const noexcept {
        return block_->rejected.load(std::memory_order_relaxed);
    }
    // What a reader would see now; the harness and tests use it.
    bool read(plugin::TelemetrySnapshot& out) const noexcept { return block_->read(out); }

private:
    plugin::TelemetryBlock local_{};
    plugin::TelemetryBlock* block_;
};

}  // namespace sonora::native
