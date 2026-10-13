// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

#include "sonora/rt.h"

namespace sonora::drivers {

enum class Fatal : uint32_t {
    None = 0,
    ServerGone,
    DeviceGone,     // a physical capture port feeding an input went away
    Stalled,        // no period for kStallNs while active
    RateChanged,    // detail: the new rate
    BufferChanged,  // detail: the refused period's size
};

// Decides whether a period may reach the engine. Once anything fatal has
// happened, nothing more does: a block at a rate or size the take was not
// started with must not be written into it (ADR-0007 §4.2).
class PeriodGuard {
public:
    static constexpr uint64_t kStallNs = 1000000000ull;

    // Before activation, on the control thread.
    void expect_rate(uint32_t rate) noexcept {
        rate_.store(rate, std::memory_order_relaxed);
    }
    // The most frames the backend's buffers hold, scratch included.
    void set_max_frames(uint32_t frames) noexcept {
        max_frames_.store(frames, std::memory_order_relaxed);
    }

    uint32_t expected_rate() const noexcept {
        return rate_.load(std::memory_order_relaxed);
    }

    // From a server notification. JACK also calls it with the rate it
    // already has, which is not a change.
    void rate_reported(uint32_t rate) noexcept {
        if (rate != expected_rate()) raise(Fatal::RateChanged, rate);
    }

    // Any thread. The first reason sticks, with its own detail: later ones
    // are consequences. Reason and detail share one word so a reader never
    // pairs one raise's reason with another's detail.
    void raise(Fatal why, uint32_t detail = 0) noexcept {
        uint64_t none = 0;
        state_.compare_exchange_strong(
            none, (static_cast<uint64_t>(why) << 32) | detail,
            std::memory_order_acq_rel);
    }

    struct State {
        Fatal why;
        uint32_t detail;
    };
    // A reader that wants both must take them from one load: fatal() then
    // detail() can pair "none yet" with a raise that landed in between.
    State state() const noexcept {
        const uint64_t s = state_.load(std::memory_order_acquire);
        return {static_cast<Fatal>(s >> 32), static_cast<uint32_t>(s)};
    }
    Fatal fatal() const noexcept { return state().why; }
    uint32_t detail() const noexcept { return state().detail; }

    // The size the first admitted period set; 0 before it. Any thread.
    uint32_t frames() const noexcept {
        return frames_.load(std::memory_order_relaxed);
    }
    // Periods the server ran, refused ones included.
    uint64_t periods() const noexcept {
        return periods_.load(std::memory_order_relaxed);
    }

    // Top of every period, on the audio thread, with the rate the server is
    // running this period at. Checking the rate here, not only on the
    // server's notification, is what makes the first period at a new rate
    // the one refused. The size is taken from the first period rather than
    // from what the server said before activation, which PipeWire's JACK
    // layer can answer with a default.
    bool admit(uint32_t nframes, uint32_t rate) noexcept SONORA_RT {
        // The audio thread is the only writer.
        periods_.store(periods_.load(std::memory_order_relaxed) + 1,
                       std::memory_order_relaxed);
        if (fatal() != Fatal::None) return false;
        if (rate != expected_rate()) {
            raise(Fatal::RateChanged, rate);
            return false;
        }
        uint32_t frames = frames_.load(std::memory_order_relaxed);
        if (frames == 0) {
            if (nframes > max_frames_.load(std::memory_order_relaxed)) {
                raise(Fatal::BufferChanged, nframes);
                return false;
            }
            frames = nframes;
            frames_.store(frames, std::memory_order_relaxed);
        }
        if (nframes != frames) {
            raise(Fatal::BufferChanged, nframes);
            return false;
        }
        return true;
    }

    // Stall watch, from the control thread: at activation, at deactivation,
    // and on each poll. Two pollers at once only blur the timing.
    void watch_from(uint64_t now_ns) noexcept {
        seen_periods_.store(periods(), std::memory_order_relaxed);
        seen_at_ns_.store(now_ns, std::memory_order_relaxed);
        watching_.store(true, std::memory_order_release);
    }
    void unwatch() noexcept { watching_.store(false, std::memory_order_release); }

    // Raises Stalled once no period has arrived for kStallNs. Returns
    // whether anything fatal has happened.
    bool check_stall(uint64_t now_ns) noexcept {
        if (watching_.load(std::memory_order_acquire) && fatal() == Fatal::None) {
            const uint64_t p = periods();
            if (p != seen_periods_.load(std::memory_order_relaxed)) {
                seen_periods_.store(p, std::memory_order_relaxed);
                seen_at_ns_.store(now_ns, std::memory_order_relaxed);
            } else {
                // Another poller may have stored a later time than ours.
                const uint64_t seen = seen_at_ns_.load(std::memory_order_relaxed);
                if (now_ns > seen && now_ns - seen >= kStallNs) {
                    raise(Fatal::Stalled);
                }
            }
        }
        return fatal() != Fatal::None;
    }

private:
    std::atomic<uint64_t> state_{0};
    std::atomic<uint32_t> rate_{0};
    std::atomic<uint32_t> max_frames_{UINT32_MAX};
    std::atomic<uint32_t> frames_{0};
    std::atomic<uint64_t> periods_{0};
    std::atomic<bool> watching_{false};
    std::atomic<uint64_t> seen_periods_{0};
    std::atomic<uint64_t> seen_at_ns_{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the audio thread reads and writes it every period");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "the audio thread reads and writes it every period");

}  // namespace sonora::drivers
