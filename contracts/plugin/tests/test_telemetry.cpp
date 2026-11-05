// SPDX-License-Identifier: GPL-3.0-or-later
//
// The telemetry region (ADR-0057): layout, the seqlock round trip, a torn
// read never returned, a scribbling plugin's words bounded, and no allocation
// on the writer's path.
#include "sonora/plugin/telemetry.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <thread>
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
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace sonora::plugin;

namespace {

struct Region {
    Region() : raw(static_cast<char*>(::operator new(telemetry_region_size() + kCacheLine))) {
        base = raw + (kCacheLine - reinterpret_cast<std::uintptr_t>(raw) % kCacheLine) % kCacheLine;
        telemetry_init(base, 3);
    }
    ~Region() { ::operator delete(raw); }
    char* raw;
    char* base;
};

void layout() {
    CHECK(telemetry_region_size() == telemetry_block_offset(kMaxInstances));
    CHECK(telemetry_block_offset(1) - telemetry_block_offset(0) == sizeof(TelemetryBlock));
    CHECK(telemetry_block_offset(0) % kCacheLine == 0);
    Region r;
    uint32_t n = 0;
    CHECK(telemetry_header_valid(r.base, n) && n == 3);
    CHECK(telemetry_block(r.base, kMaxInstances) == nullptr);
    reinterpret_cast<TelemetryHeader*>(r.base)->version = kTelemetryVersion + 1;
    CHECK(!telemetry_header_valid(r.base, n));
    reinterpret_cast<TelemetryHeader*>(r.base)->version = kTelemetryVersion;
    reinterpret_cast<TelemetryHeader*>(r.base)->instance_count = kMaxInstances + 1;
    CHECK(!telemetry_header_valid(r.base, n));
}

void round_trip() {
    Region r;
    TelemetryBlock* b = telemetry_block(r.base, 0);
    TelemetrySnapshot s;
    CHECK(!b->read(s));  // never written
    float curve[kMaxCurvePoints];
    for (uint32_t i = 0; i < kMaxCurvePoints; ++i) curve[i] = static_cast<float>(i) * 0.5f;
    b->begin_write();
    b->set_meter(0, -6.5f);
    b->set_meter(7, 0.25f);
    b->set_curve(1, curve, kMaxCurvePoints);
    b->end_write();
    CHECK(b->read(s));
    CHECK(s.sequence == 1);
    CHECK(s.meter_mask == ((1u << 0) | (1u << 7)));
    CHECK(s.meters[0] == -6.5f && s.meters[7] == 0.25f);
    CHECK(s.curve_points[0] == 0 && s.curve_points[1] == kMaxCurvePoints);
    CHECK(s.curves[1][127] == 63.5f);
    // A slot or curve past the end is ignored, never written.
    b->begin_write();
    b->set_meter(kMaxTelemetryMeters, 1.0f);
    b->set_curve(kMaxTelemetryCurves, curve, 4);
    b->set_curve(0, curve, kMaxCurvePoints + 50);
    b->end_write();
    CHECK(b->read(s) && s.sequence == 2 && s.curve_points[0] == kMaxCurvePoints);
    // Blocks are independent.
    CHECK(!telemetry_block(r.base, 1)->read(s));
}

void non_finite_and_scribbled() {
    Region r;
    TelemetryBlock* b = telemetry_block(r.base, 2);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    float curve[4] = {1.0f, nan, inf, 4.0f};
    b->begin_write();
    b->set_meter(0, nan);
    b->set_meter(1, 2.0f);
    b->set_curve(0, curve, 4);
    b->end_write();
    TelemetrySnapshot s;
    CHECK(b->read(s));
    CHECK(s.meters[0] == 0.0f && s.curves[0][1] == 0.0f && s.curves[0][2] == 0.0f);
    CHECK(b->rejected.load() == 3);
    // A plugin that writes the words itself (it can: the region is mapped
    // writable in its host): a NaN meter loses its bit, a bad count refuses.
    b->meters[3].store(nan);
    b->meter_mask.store(0xFFFFFFFFu);
    CHECK(b->read(s));
    CHECK((s.meter_mask & (1u << 3)) == 0 && (s.meter_mask >> kMaxTelemetryMeters) == 0);
    b->curve_points[0].store(0xFFFFFFFFu);
    CHECK(!b->read(s));
}

// A reader never returns a frame that mixes two writes: the writer keeps
// every meter and curve point of a frame equal to the frame number.
void no_torn_frame() {
    Region r;
    TelemetryBlock* b = telemetry_block(r.base, 0);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        float curve[kMaxCurvePoints];
        for (uint32_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
            const float v = static_cast<float>(n);
            for (float& c : curve) c = v;
            b->begin_write();
            for (uint32_t m = 0; m < kMaxTelemetryMeters; ++m) b->set_meter(m, v);
            b->set_curve(0, curve, kMaxCurvePoints);
            b->end_write();
            // A plugin publishes once per block, not in a tight loop; one that
            // never paused would starve every reader, which is also tolerated.
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });
    long reads = 0, torn = 0;
    TelemetrySnapshot s;
    // The reader can finish its loop before the writer thread is scheduled
    // (seq still 0, every read false), so run until it has seen real frames.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int i = 0; i < 200000 || (reads < 1000 && std::chrono::steady_clock::now() < deadline); ++i) {
        if (!b->read(s)) continue;
        ++reads;
        for (uint32_t m = 0; m < kMaxTelemetryMeters; ++m) {
            if (s.meters[m] != s.meters[0]) ++torn;
        }
        for (uint32_t p = 0; p < s.curve_points[0]; ++p) {
            if (s.curves[0][p] != s.meters[0]) ++torn;
        }
    }
    stop = true;
    writer.join();
    CHECK(reads > 0);
    CHECK(torn == 0);
}

void no_allocation() {
    Region r;
    TelemetryBlock* b = telemetry_block(r.base, 0);
    float curve[kMaxCurvePoints] = {};
    const long before = g_allocations.load();
    for (int i = 0; i < 1000; ++i) {
        b->begin_write();
        for (uint32_t m = 0; m < kMaxTelemetryMeters; ++m) b->set_meter(m, 1.0f);
        b->set_curve(0, curve, kMaxCurvePoints);
        b->end_write();
    }
    CHECK(g_allocations.load() == before);
}

}  // namespace

int main() {
    layout();
    round_trip();
    non_finite_and_scribbled();
    no_torn_frame();
    no_allocation();
    if (failures == 0) std::printf("telemetry: ok\n");
    return failures == 0 ? 0 : 1;
}
