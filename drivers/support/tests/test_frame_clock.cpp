// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioBlock::frame_time never decreases. JACK's frame counter is 32 bits
// and wraps after about a day at 48 kHz (12.4 hours at 96 kHz), well inside
// how long the appliance runs, so the backend widens it on the audio thread.
#include <cstdint>
#include <cstdio>

#include "sonora/frame_clock.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::drivers::FrameClock;

int test_starts_at_the_server_count() {
    FrameClock c;
    CHECK(c.widen(1000) == 1000);
    CHECK(c.widen(1128) == 1128);
    return 0;
}

int test_carries_across_the_wrap() {
    FrameClock c;
    uint32_t now = 0xFFFFFF00u;
    uint64_t prev = c.widen(now);
    CHECK(prev == 0xFFFFFF00u);
    for (int i = 0; i < 4; ++i) {
        now += 128;  // wraps on the second step
        const uint64_t t = c.widen(now);
        CHECK(t == prev + 128);
        prev = t;
    }
    CHECK(prev == 0xFFFFFF00ull + 4 * 128);
    CHECK(prev > 0xFFFFFFFFull);
    return 0;
}

int test_wraps_twice() {
    FrameClock c;
    uint32_t now = 0;
    uint64_t t = c.widen(now);
    const uint32_t step = 0x40000000u;  // a quarter of the 32-bit range
    for (int i = 0; i < 8; ++i) {
        now += step;
        t = c.widen(now);
    }
    CHECK(t == 8ull * step);
    return 0;
}

// Lost frames show as a gap, never as time going backwards.
int test_gap_is_kept() {
    FrameClock c;
    c.widen(0xFFFFFF80u);
    CHECK(c.widen(0x00000200u) == 0xFFFFFF80ull + 0x280);
    return 0;
}

// The server's clock can step backwards: a driver restarted, the graph
// moved to another driver, the server restarted under the client. Unsigned
// subtraction read that as a forward jump of nearly 2^32 frames, about a
// day of invented time. A backward step is a clock change: the wide clock
// holds still, counts it, and runs on from the new count.
int test_backward_step_is_a_discontinuity() {
    FrameClock c;
    CHECK(c.widen(100000) == 100000);
    CHECK(c.discontinuities() == 0);
    CHECK(c.widen(5000) == 100000);
    CHECK(c.discontinuities() == 1);
    CHECK(c.widen(5128) == 100128);
    CHECK(c.widen(5256) == 100256);
    CHECK(c.discontinuities() == 1);
    return 0;
}

// A wrap is forward, and not a discontinuity.
int test_wrap_is_not_a_discontinuity() {
    FrameClock c;
    c.widen(0xFFFFFF80u);
    c.widen(0x00000000u);
    CHECK(c.discontinuities() == 0);
    return 0;
}

// PipeWire's dummy driver, woken late, skips a period: jack_last_frame_time
// steps by two periods and no xrun is reported (measured in the docker
// test's container, 2026-10-13: 26 of 26 skips, xruns 0). Unseen, the run
// reported a clean clock while the server had not run for a period. It is a
// discontinuity, and frame_time keeps the gap.
int test_unreported_skip_is_a_discontinuity() {
    FrameClock c;
    CHECK(c.widen(1000, 128, 0) == 1000);
    CHECK(c.widen(1128, 128, 0) == 1128);
    CHECK(c.discontinuities() == 0);
    CHECK(c.widen(1384, 128, 0) == 1384);
    CHECK(c.discontinuities() == 1);
    CHECK(c.widen(1512, 128, 0) == 1512);
    CHECK(c.discontinuities() == 1);
    return 0;
}

// An xrun the server reported is the gap's excuse already.
int test_reported_xrun_is_not_a_discontinuity() {
    FrameClock c;
    c.widen(1000, 128, 0);
    c.widen(1128, 128, 0);
    CHECK(c.widen(2128, 128, 1) == 2128);
    CHECK(c.discontinuities() == 0);
    return 0;
}

// The step that follows a period is that period's length, whatever the
// next one's is.
int test_step_of_the_previous_period_is_not_a_skip() {
    FrameClock c;
    c.widen(0, 256, 0);
    c.widen(256, 128, 0);
    c.widen(384, 128, 0);
    CHECK(c.discontinuities() == 0);
    return 0;
}

// The server's clock runs while the client is deactivated; the first step
// after a reactivation is not a skip.
int test_step_across_a_reactivation_is_not_a_skip() {
    FrameClock c;
    c.widen(1000, 128, 0);
    c.resume();
    CHECK(c.widen(480000, 128, 0) == 480000);
    CHECK(c.discontinuities() == 0);
    c.widen(480384, 128, 0);
    CHECK(c.discontinuities() == 1);
    return 0;
}

}  // namespace

int main() {
    if (test_starts_at_the_server_count() != 0) return 1;
    if (test_carries_across_the_wrap() != 0) return 1;
    if (test_wraps_twice() != 0) return 1;
    if (test_gap_is_kept() != 0) return 1;
    if (test_backward_step_is_a_discontinuity() != 0) return 1;
    if (test_wrap_is_not_a_discontinuity() != 0) return 1;
    if (test_unreported_skip_is_a_discontinuity() != 0) return 1;
    if (test_reported_xrun_is_not_a_discontinuity() != 0) return 1;
    if (test_step_of_the_previous_period_is_not_a_skip() != 0) return 1;
    if (test_step_across_a_reactivation_is_not_a_skip() != 0) return 1;
    std::printf("test_frame_clock: ok\n");
    return 0;
}
