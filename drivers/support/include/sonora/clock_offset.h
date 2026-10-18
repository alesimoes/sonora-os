// SPDX-License-Identifier: GPL-3.0-or-later
//
// The offset from a server's clock to this process's steady_clock, kept off
// the audio thread from brackets: steady read, server read, steady read. The
// server read sits somewhere inside the bracket, so the middle is off by at
// most half its width; a bracket the scheduler stretched is that much wrong.
// JackBackend keeps one for jack_get_time(), whose base is not necessarily
// CLOCK_MONOTONIC (jackd2's is CLOCK_MONOTONIC_RAW; PR #77).
//
// A value from a bracket wider than kMaxWidthNs is provisional: taken
// because there is nothing better, replaced by any narrower bracket, and by
// the first narrow one for good. Once a narrow bracket has been seen, wide
// ones are ignored. So a bad first value cannot persist through a run in
// which every later bracket is wide (PR #77 RT audit L1).
#pragma once

#include <cstdint>

namespace sonora::drivers {

class ClockOffset {
public:
    // The brackets measured in the container were under 30 us, idle or
    // under load; 100 us is a scheduler's stretch, not a clock read.
    static constexpr int64_t kMaxWidthNs = 100'000;
    // Tries for the first value before the narrowest so far is kept.
    static constexpr int kFirstTries = 10;

    struct Bracket {
        int64_t t0, server, t1;
    };

    // One bracket, in ns. True when the offset changed.
    bool offer(int64_t steady_before, int64_t server, int64_t steady_after) noexcept {
        const int64_t width = steady_after - steady_before;
        if (width < 0) return false;
        const bool narrow = width <= kMaxWidthNs;
        if (have_ && (trusted_ ? !narrow : !narrow && width >= width_)) return false;
        offset_ = steady_before + width / 2 - server;
        width_ = width;
        have_ = true;
        trusted_ = narrow;
        return true;
    }
    template <class Bracket>
    bool offer(const Bracket& b) noexcept {
        return offer(b.t0, b.server, b.t1);
    }

    // The first value: up to kFirstTries brackets from `next`, stopping at
    // the first narrow one. `next` returns something with t0, server, t1.
    template <class Next>
    void first(Next&& next) {
        for (int i = 0; i < kFirstTries && !trusted_; ++i) offer(next());
    }

    bool have() const noexcept { return have_; }
    bool trusted() const noexcept { return trusted_; }
    int64_t offset() const noexcept { return offset_; }

private:
    int64_t offset_ = 0;
    int64_t width_ = 0;
    bool have_ = false;
    bool trusted_ = false;
};

}  // namespace sonora::drivers
