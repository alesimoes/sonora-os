// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/thread_cpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    using namespace std::chrono;
    const auto t = sonora::platform::thread_cpu_times();
    if (!t.available) {
        // The unsupported implementation must say why, never just be empty.
        check(!t.unavailable_reason.empty(), "unavailable carries a reason");
        check(t.threads.empty(), "unavailable reports no threads");
        return failures ? 1 : 0;
    }
    check(t.resolution_ns > 0, "resolution is positive");
    check(!t.threads.empty(), "the calling thread is listed");

    // A thread that spins for many ticks must show it, so reading the wrong
    // /proc columns fails here rather than in a report nobody checks.
    std::atomic<bool> stop{false};
    std::thread burner([&stop] {
        volatile uint64_t x = 0;
        while (!stop.load(std::memory_order_relaxed)) x = x + 1;
    });
    const auto until = steady_clock::now() + milliseconds(300);
    while (steady_clock::now() < until) std::this_thread::sleep_for(milliseconds(10));
    const auto after = sonora::platform::thread_cpu_times();
    stop.store(true);
    burner.join();

    int64_t max_ns = 0;
    for (const auto& th : after.threads) max_ns = std::max(max_ns, th.cpu_ns);
    check(after.threads.size() >= 2, "the spinning thread is listed");
    check(max_ns >= 100'000'000, "a spinning thread shows at least 100 ms of CPU");
    check(max_ns <= 10'000'000'000, "and not an absurd figure");
    return failures ? 1 : 0;
}
