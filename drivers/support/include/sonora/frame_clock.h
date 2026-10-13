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
    // `nframes` is this period's length and `xruns` the server's xrun count
    // so far; with them a skipped period is told from a reported xrun.
    // nframes 0 judges no step.
    uint64_t widen(uint32_t now, uint32_t nframes = 0, uint32_t xruns = 0) noexcept SONORA_RT {
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
            const bool resumed = resumed_.exchange(false, std::memory_order_acquire);
            if (step > kMaxForward) {
                count_discontinuity();
            } else {
                // Longer than the last period with no xrun to excuse it:
                // the server did not run a period and did not say so.
                // PipeWire's dummy driver does this when woken late. The gap
                // stays in frame_time; only the excuse was missing.
                if (!resumed && last_nframes_ != 0 && step > last_nframes_ &&
                    xruns == last_xruns_) {
                    count_discontinuity();
                }
                wide_ += step;
            }
        }
        last_ = now;
        last_nframes_ = nframes;
        last_xruns_ = xruns;
        return wide_;
    }

    // The server's clock ran on while no period reached us (a deactivation),
    // so the next step is not judged a skip. Before the periods resume.
    void resume() noexcept { resumed_.store(true, std::memory_order_release); }

    // Backward steps, and forward skips no xrun explains, seen so far. Any
    // thread.
    uint32_t discontinuities() const noexcept {
        return discontinuities_.load(std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMaxForward = 0x7FFFFFFFu;

    void count_discontinuity() noexcept SONORA_RT {
        discontinuities_.store(discontinuities_.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
    }

    bool started_ = false;
    uint32_t last_ = 0;
    uint32_t last_nframes_ = 0;
    uint32_t last_xruns_ = 0;
    uint64_t wide_ = 0;
    std::atomic<bool> resumed_{false};
    // The audio thread is the only writer.
    std::atomic<uint32_t> discontinuities_{0};
};

}  // namespace sonora::drivers
