// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

#include "sonora/rt.h"

namespace sonora::drivers {

// Widens a server's 32-bit frame counter to AudioBlock::frame_time's 64 bits.
// JACK's wraps after about a day at 48 kHz. widen() is audio thread only: it
// is called once per period.
class FrameClock {
public:
    uint64_t widen(uint32_t now) noexcept SONORA_RT {
        if (!started_) {
            started_ = true;
            wide_ = now;
        } else {
            // Unsigned subtraction is the forward distance even across a
            // wrap. A distance past half the range cannot be one: 2^31
            // frames is over twelve hours between two periods. It is the
            // counter going backwards, which no number of frames explains,
            // so the clock holds still rather than inventing a day.
            const uint32_t step = now - last_;
            if (step > kMaxForward) {
                discontinuities_.store(
                    discontinuities_.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            } else {
                wide_ += step;
            }
        }
        last_ = now;
        return wide_;
    }

    // Backward steps seen so far. Any thread.
    uint32_t discontinuities() const noexcept {
        return discontinuities_.load(std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMaxForward = 0x7FFFFFFFu;

    bool started_ = false;
    uint32_t last_ = 0;
    uint64_t wide_ = 0;
    // The audio thread is the only writer.
    std::atomic<uint32_t> discontinuities_{0};
};

}  // namespace sonora::drivers
