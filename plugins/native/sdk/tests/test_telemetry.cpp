// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDK's telemetry publisher (ADR-0057): an unbound processor publishes
// into a private block, a bound one into the host's, a frame reads back
// whole, and publishing allocates nothing (counted here; the RTSan build
// also aborts on any allocation, lock or syscall inside the SONORA_RT calls).
#include "sonora/native/telemetry.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {
std::atomic<long> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size > 0 ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace sonora;

int main() {
    // Unbound: the processor runs the same code, and the harness can read it.
    {
        native::Telemetry t;
        plugin::TelemetrySnapshot s;
        CHECK(!t.bound() && !t.read(s));
        t.begin();
        t.meter(0, 4.5f);
        t.end();
        CHECK(t.read(s) && s.meters[0] == 4.5f && (s.meter_mask & 1u) != 0);
    }
    // Bound to the host's block: the writes land there, a reader of that
    // block sees them, and unbinding goes back to the private one.
    {
        plugin::TelemetryBlock shared{};
        native::Telemetry t;
        t.bind(&shared);
        CHECK(t.bound());
        std::vector<float> curve(plugin::kMaxCurvePoints);
        for (std::size_t i = 0; i < curve.size(); ++i) curve[i] = static_cast<float>(i);
        t.begin();
        t.meter(1, -3.0f);
        t.meter(2, std::numeric_limits<float>::quiet_NaN());
        t.curve(0, curve.data(), static_cast<std::uint32_t>(curve.size()));
        t.end();
        plugin::TelemetrySnapshot s;
        CHECK(shared.read(s) && s.meters[1] == -3.0f && s.meters[2] == 0.0f);
        CHECK(s.curve_points[0] == plugin::kMaxCurvePoints && s.curves[0][100] == 100.0f);
        CHECK(t.rejected() == 1);
        t.bind(nullptr);
        CHECK(!t.bound());
        t.begin();
        t.meter(0, 9.0f);
        t.end();
        CHECK(shared.read(s) && s.meters[0] == 0.0f);   // the host's block was not touched
    }
    // The publish path: no allocation.
    {
        native::Telemetry t;
        std::vector<float> curve(plugin::kMaxCurvePoints, 1.0f);
        const long before = g_allocations.load();
        for (int i = 0; i < 5000; ++i) {
            t.begin();
            for (std::uint32_t m = 0; m < plugin::kMaxTelemetryMeters; ++m) t.meter(m, 0.5f);
            t.curve(0, curve.data(), plugin::kMaxCurvePoints);
            t.curve(1, curve.data(), 64);
            t.end();
        }
        CHECK(g_allocations.load() == before);
    }
    if (failures) return EXIT_FAILURE;
    std::printf("native telemetry: ok\n");
    return EXIT_SUCCESS;
}
