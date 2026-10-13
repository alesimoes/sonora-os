// SPDX-License-Identifier: GPL-3.0-or-later
//
// A rate or buffer size change mid-session is fatal (ADR-0007 §4.2): the
// backend stops handing blocks to the engine, and says why, rather than
// recording on at a size or rate the take was not started with. PeriodGuard
// is the part of JackBackend that decides this. It needs no server, so it is
// tested here; the rest of the backend is exercised by
// tools/test-engine-docker.sh.
#include <atomic>
#include <cstdio>
#include <thread>

#include "period_guard.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::jack_detail::Fatal;
using sonora::jack_detail::PeriodGuard;

int test_steady_periods_pass() {
    PeriodGuard g;
    g.expect_rate(48000);
    for (int i = 0; i < 1000; ++i) CHECK(g.admit(128));
    CHECK(g.fatal() == Fatal::None);
    return 0;
}

// The size is learned from the first period, not from what the server said
// before activation: PipeWire's JACK layer can report a default there.
int test_first_period_sets_the_size() {
    PeriodGuard g;
    CHECK(g.admit(256));
    CHECK(g.admit(256));
    CHECK(g.fatal() == Fatal::None);
    return 0;
}

int test_buffer_change_is_fatal_and_final() {
    PeriodGuard g;
    CHECK(g.admit(128));
    CHECK(!g.admit(64));
    CHECK(g.fatal() == Fatal::BufferChanged);
    // Going back does not make the take whole again.
    CHECK(!g.admit(128));
    CHECK(g.fatal() == Fatal::BufferChanged);
    return 0;
}

int test_rate_change_is_fatal() {
    PeriodGuard g;
    g.expect_rate(48000);
    g.rate_reported(48000);  // JACK reports the current rate when asked to
    CHECK(g.fatal() == Fatal::None);
    CHECK(g.admit(128));
    g.rate_reported(44100);
    CHECK(g.fatal() == Fatal::RateChanged);
    CHECK(!g.admit(128));
    return 0;
}

int test_server_loss_stops_blocks() {
    PeriodGuard g;
    CHECK(g.admit(128));
    g.raise(Fatal::ServerGone);
    CHECK(!g.admit(128));
    CHECK(g.fatal() == Fatal::ServerGone);
    return 0;
}

// The reason the operator reads is the first thing that went wrong, not
// whatever followed from it.
int test_first_reason_wins() {
    PeriodGuard g;
    g.expect_rate(48000);
    g.rate_reported(96000);
    g.raise(Fatal::ServerGone);
    CHECK(!g.admit(128));
    CHECK(g.fatal() == Fatal::RateChanged);
    return 0;
}

// The audio thread admits while another thread raises, as JACK's shutdown
// and sample-rate callbacks do. Run under TSan this is the race check.
int test_raise_from_another_thread() {
    PeriodGuard g;
    g.expect_rate(48000);
    std::atomic<bool> started{false};
    std::atomic<long> admitted{0};
    std::thread rt([&] {
        long n = 0;
        while (g.admit(128)) {
            if (++n == 1) started.store(true, std::memory_order_release);
        }
        admitted.store(n, std::memory_order_relaxed);
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    g.rate_reported(44100);
    rt.join();
    CHECK(admitted.load(std::memory_order_relaxed) >= 1);
    CHECK(g.fatal() == Fatal::RateChanged);
    return 0;
}

}  // namespace

int main() {
    if (test_steady_periods_pass() != 0) return 1;
    if (test_first_period_sets_the_size() != 0) return 1;
    if (test_buffer_change_is_fatal_and_final() != 0) return 1;
    if (test_rate_change_is_fatal() != 0) return 1;
    if (test_server_loss_stops_blocks() != 0) return 1;
    if (test_first_reason_wins() != 0) return 1;
    if (test_raise_from_another_thread() != 0) return 1;
    std::printf("test_period_guard: ok\n");
    return 0;
}
