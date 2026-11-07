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

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sonora/plugin/telemetry.h"
#include "sonora/rt.h"

namespace sonora::native {

class Telemetry {
public:
    Telemetry() noexcept : block_(&local_) { enrol(this); }
    // For an instance that offers itself to the host only if it publishes
    // (ProcessorBase::publish_levels calls offer()): a bundle whose processor
    // never publishes must answer "nothing to bind" to the host's hand-over.
    struct Deferred {};
    explicit Telemetry(Deferred) noexcept : block_(&local_) {}
    ~Telemetry() { withdraw(this); }
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;
    // How a plugin host hands the instance it just created its shared block:
    // the plugin pulls it from the host in its constructor (take_host_telemetry_block,
    // format/src/dpf/plugin.cpp) and calls this. Binds the
    // oldest instance made since the last call and leaves the rest private:
    // a mono plugin run once per channel has several, and only one may write
    // the insert's block. True when one was bound. Control thread, before
    // audio, while the host instantiates one plugin at a time.
    static bool bind_pending(void* shared_block) noexcept {
        auto* slots = pending();
        Telemetry* first = nullptr;
        for (std::size_t i = 0; i < kPending; ++i) {
            Telemetry* t = slots[i].exchange(nullptr);
            if (first == nullptr) first = t;
        }
        if (first == nullptr || shared_block == nullptr) return false;
        first->bind(static_cast<plugin::TelemetryBlock*>(shared_block));
        return true;
    }

    // Makes this instance a candidate for bind_pending(). Idempotent.
    void offer() noexcept {
        withdraw(this);
        enrol(this);
    }
    // The opposite, for an instance embedded in another processor (a stage of
    // a chain): it is not the insert, so the host must not bind it.
    void withhold() noexcept { withdraw(this); }

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
    static constexpr std::size_t kPending = 8;
    static std::atomic<Telemetry*>* pending() noexcept {
        static std::atomic<Telemetry*> slots[kPending] = {};
        return slots;
    }
    static void enrol(Telemetry* t) noexcept {
        auto* slots = pending();
        for (std::size_t i = 0; i < kPending; ++i) {
            Telemetry* none = nullptr;
            if (slots[i].compare_exchange_strong(none, t)) return;
        }
    }
    static void withdraw(Telemetry* t) noexcept {
        auto* slots = pending();
        for (std::size_t i = 0; i < kPending; ++i) {
            Telemetry* me = t;
            slots[i].compare_exchange_strong(me, nullptr);
        }
    }

    plugin::TelemetryBlock local_{};
    plugin::TelemetryBlock* block_;
};

}  // namespace sonora::native
