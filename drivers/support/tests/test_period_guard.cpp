// SPDX-License-Identifier: GPL-3.0-or-later
//
// A rate or buffer size change mid-session is fatal (ADR-0007 §4.2), and so
// are a lost device and a server that stops running the client: the backend
// stops handing blocks to the engine and says why, rather than recording on
// at a size or rate the take was not started with. PeriodGuard is the part of
// a backend that decides this. It needs no server, so it is tested here;
// JackBackend's use of it is exercised by tools/test-engine-docker.sh.
#include <atomic>
#include <cstdio>
#include <thread>

#include "sonora/period_guard.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::drivers::Fatal;
using sonora::drivers::PeriodGuard;

constexpr uint32_t kRate = 48000;
constexpr uint64_t kSecond = 1000000000ull;

int test_steady_periods_pass() {
    PeriodGuard g;
    g.expect_rate(kRate);
    for (int i = 0; i < 1000; ++i) CHECK(g.admit(128, kRate));
    CHECK(g.fatal() == Fatal::None);
    CHECK(g.periods() == 1000);
    return 0;
}

// The size is learned from the first period, not from what the server said
// before activation: PipeWire's JACK layer can report a default there.
int test_first_period_sets_the_size() {
    PeriodGuard g;
    g.expect_rate(kRate);
    CHECK(g.frames() == 0);
    CHECK(g.admit(256, kRate));
    CHECK(g.frames() == 256);
    CHECK(g.admit(256, kRate));
    CHECK(g.fatal() == Fatal::None);
    return 0;
}

int test_buffer_change_is_fatal_and_final() {
    PeriodGuard g;
    g.expect_rate(kRate);
    CHECK(g.admit(128, kRate));
    CHECK(!g.admit(64, kRate));
    CHECK(g.fatal() == Fatal::BufferChanged);
    CHECK(g.detail() == 64);
    CHECK(g.frames() == 128);
    // Going back does not make the take whole again.
    CHECK(!g.admit(128, kRate));
    CHECK(g.fatal() == Fatal::BufferChanged);
    return 0;
}

// The backend's buffers hold so many frames; a period larger than that is
// refused rather than handed over, even as the first one.
int test_period_larger_than_the_buffers_is_fatal() {
    PeriodGuard g;
    g.expect_rate(kRate);
    g.set_max_frames(1024);
    CHECK(!g.admit(2048, kRate));
    CHECK(g.fatal() == Fatal::BufferChanged);
    CHECK(g.detail() == 2048);
    CHECK(g.frames() == 0);
    return 0;
}

// Checked in the period itself, so the first block at the new rate is the
// one refused: the notification that the rate changed can arrive later.
int test_rate_change_is_caught_in_the_period() {
    PeriodGuard g;
    g.expect_rate(kRate);
    CHECK(g.admit(128, kRate));
    CHECK(!g.admit(128, 96000));
    CHECK(g.fatal() == Fatal::RateChanged);
    CHECK(g.detail() == 96000);
    CHECK(g.expected_rate() == kRate);
    return 0;
}

int test_rate_change_reported_by_the_server() {
    PeriodGuard g;
    g.expect_rate(kRate);
    g.rate_reported(kRate);  // JACK reports the current rate when asked to
    CHECK(g.fatal() == Fatal::None);
    CHECK(g.admit(128, kRate));
    g.rate_reported(44100);
    CHECK(g.fatal() == Fatal::RateChanged);
    CHECK(g.detail() == 44100);
    CHECK(!g.admit(128, kRate));
    return 0;
}

int test_server_loss_stops_blocks() {
    PeriodGuard g;
    g.expect_rate(kRate);
    CHECK(g.admit(128, kRate));
    g.raise(Fatal::ServerGone);
    CHECK(!g.admit(128, kRate));
    CHECK(g.fatal() == Fatal::ServerGone);
    return 0;
}

// The reason the operator reads is the first thing that went wrong, not
// whatever followed from it, and its detail is that reason's detail.
int test_first_reason_wins() {
    PeriodGuard g;
    g.expect_rate(kRate);
    g.rate_reported(96000);
    g.raise(Fatal::BufferChanged, 64);
    g.raise(Fatal::ServerGone);
    CHECK(!g.admit(128, kRate));
    CHECK(g.fatal() == Fatal::RateChanged);
    CHECK(g.detail() == 96000);
    return 0;
}

// No period for a second while active: the server has stopped running the
// client, and a take that waits on it waits forever.
int test_stall_is_fatal() {
    PeriodGuard g;
    g.expect_rate(kRate);
    uint64_t now = 5 * kSecond;
    g.watch_from(now);
    for (int i = 0; i < 10; ++i) {
        CHECK(g.admit(128, kRate));
        now += kSecond / 2;  // a slow poller still sees periods arriving
        CHECK(!g.check_stall(now));
    }
    now += kSecond - 1;
    CHECK(!g.check_stall(now));
    now += 1;
    CHECK(g.check_stall(now));
    CHECK(g.fatal() == Fatal::Stalled);
    CHECK(!g.admit(128, kRate));
    return 0;
}

// Before activation and after deactivation nothing calls the client, and
// that is not a stall.
int test_no_stall_unless_watched() {
    PeriodGuard g;
    g.expect_rate(kRate);
    CHECK(!g.check_stall(100 * kSecond));
    g.watch_from(100 * kSecond);
    g.unwatch();
    CHECK(!g.check_stall(200 * kSecond));
    CHECK(g.fatal() == Fatal::None);
    return 0;
}

// The audio thread admits while other threads raise and poll, as JACK's
// shutdown and sample-rate callbacks and the control thread do. Run under
// TSan this is the race check.
int test_raise_from_another_thread() {
    PeriodGuard g;
    g.expect_rate(kRate);
    g.watch_from(0);
    std::atomic<bool> started{false};
    std::atomic<long> admitted{0};
    std::thread rt([&] {
        long n = 0;
        while (g.admit(128, kRate)) {
            if (++n == 1) started.store(true, std::memory_order_release);
        }
        admitted.store(n, std::memory_order_relaxed);
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    (void)g.check_stall(1);
    (void)g.frames();
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
    if (test_period_larger_than_the_buffers_is_fatal() != 0) return 1;
    if (test_rate_change_is_caught_in_the_period() != 0) return 1;
    if (test_rate_change_reported_by_the_server() != 0) return 1;
    if (test_server_loss_stops_blocks() != 0) return 1;
    if (test_first_reason_wins() != 0) return 1;
    if (test_stall_is_fatal() != 0) return 1;
    if (test_no_stall_unless_watched() != 0) return 1;
    if (test_raise_from_another_thread() != 0) return 1;
    std::printf("test_period_guard: ok\n");
    return 0;
}
