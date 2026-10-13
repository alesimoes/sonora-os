// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

#include "sonora/rt.h"

namespace sonora::drivers {

enum class Fatal : uint32_t {
    None = 0,
    ServerGone,
    RateChanged,
    BufferChanged,
};

// Decides whether a period may reach the engine. Once anything fatal has
// happened, nothing more does: a block at a rate or size the take was not
// started with must not be written into it (ADR-0007 §4.2).
class PeriodGuard {
public:
    // Before activation, on the control thread.
    void expect_rate(uint32_t rate) noexcept {
        rate_.store(rate, std::memory_order_relaxed);
    }

    // From JACK's sample-rate callback. JACK also calls it with the rate it
    // already has, which is not a change.
    void rate_reported(uint32_t rate) noexcept {
        if (rate != rate_.load(std::memory_order_relaxed)) raise(Fatal::RateChanged);
    }

    // Any thread. The first reason sticks: later ones are consequences.
    void raise(Fatal why) noexcept {
        Fatal none = Fatal::None;
        fatal_.compare_exchange_strong(none, why, std::memory_order_acq_rel);
    }

    Fatal fatal() const noexcept { return fatal_.load(std::memory_order_acquire); }

    // Top of every period, on the audio thread. The size is taken from the
    // first period rather than from jack_get_buffer_size() before
    // activation, which PipeWire's JACK layer can answer with a default;
    // what a take must not see is the size changing under it.
    bool admit(uint32_t nframes) noexcept SONORA_RT {
        if (fatal_.load(std::memory_order_acquire) != Fatal::None) return false;
        // Only the audio thread touches frames_, one period at a time.
        if (frames_ == 0) frames_ = nframes;
        if (nframes != frames_) {
            raise(Fatal::BufferChanged);
            return false;
        }
        return true;
    }

private:
    std::atomic<Fatal> fatal_{Fatal::None};
    std::atomic<uint32_t> rate_{0};
    uint32_t frames_ = 0;
};

static_assert(std::atomic<Fatal>::is_always_lock_free,
              "the audio thread reads it every period");

}  // namespace sonora::drivers
