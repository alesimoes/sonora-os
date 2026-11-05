// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin TELEMETRY region, layout version 1 (ADR-0057): what a plugin
// shows its editor that is not a parameter -- gain reduction, input and
// output level, and a magnitude-response curve -- carried from the plugin's
// process() to the engine's control thread, read-only on the engine's side.
//
// A THIRD region, beside the channel and clock regions of protocol.h, so
// that kLayoutVersion and every host already built are untouched. One per
// host process, kMaxInstances blocks, block n belonging to chain instance n.
// The engine creates it, hands the host a read-write descriptor and keeps
// its own PROT_READ mapping (the memfd path the clock region uses).
//
// Writer: the plugin's audio thread, through a SEQLOCK built from atomics:
// no lock, no allocation, no syscall, wait-free (a bounded number of
// stores). Reader: the engine's CONTROL thread, never the audio thread. It
// retries a torn read a bounded number of times and otherwise keeps the last
// frame it had; it never waits for the plugin.
//
// Trust (protocol.h, "Trust"): the host process runs third-party code with
// this region mapped writable, so every word is untrusted when read. The
// reader checks the version and sizes, bounds every count before it indexes
// with it, and replaces a non-finite value; it never turns a word of the
// region into an offset or a pointer.
//
// Platform-neutral (ADR-0008): fixed-width integers, std::atomic and offsets.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>

#include "sonora/plugin/protocol.h"
#include "sonora/rt.h"

namespace sonora::plugin {

// Moves with any change to the structs below; the header carries it and a
// reader refuses a region of another version.
inline constexpr uint32_t kTelemetryVersion = 1;
inline constexpr uint32_t kTelemetryMagic = 0x4D454C53;  // "SLEM"
inline constexpr uint32_t kMaxTelemetryMeters = 8;
inline constexpr uint32_t kMaxTelemetryCurves = 2;
inline constexpr uint32_t kMaxCurvePoints = 128;
// How many times a reader retries a read that overlapped a write before it
// gives up and keeps the previous frame.
inline constexpr int kTelemetryReadAttempts = 8;

// One reading, copied out of a block. Plain data; lives on the reader's stack.
struct TelemetrySnapshot {
    uint32_t sequence = 0;    // finished frames the writer had counted; 0: never written
    uint32_t meter_mask = 0;  // bit n set: meters[n] holds a published, finite value
    float meters[kMaxTelemetryMeters] = {};
    uint32_t curve_points[kMaxTelemetryCurves] = {};  // 0: no curve
    float curves[kMaxTelemetryCurves][kMaxCurvePoints] = {};
};

// One insert's block. Every field is an atomic so that a read that overlaps
// a write is not a data race; the seqlock makes the copy consistent.
struct TelemetryBlock {
    // Odd while the writer is inside a frame; seq / 2 counts finished frames.
    alignas(kCacheLine) std::atomic<uint32_t> seq;
    std::atomic<uint32_t> meter_mask;
    // Values the writer was handed that were not finite, replaced by 0.
    std::atomic<uint32_t> rejected;
    std::atomic<uint32_t> curve_points[kMaxTelemetryCurves];
    std::atomic<float> meters[kMaxTelemetryMeters];
    std::atomic<float> curves[kMaxTelemetryCurves][kMaxCurvePoints];

    // --- writer (the plugin's audio thread; one writer per block) ----------
    // begin/end bracket one frame; set_* go between them.
    void begin_write() noexcept SONORA_RT {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        // The odd value must be visible before any field below changes.
        std::atomic_thread_fence(std::memory_order_release);
    }
    void end_write() noexcept SONORA_RT {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);
    }
    void set_meter(uint32_t slot, float value) noexcept SONORA_RT {
        if (slot >= kMaxTelemetryMeters) return;
        const bool ok = std::isfinite(value);
        if (!ok) rejected.fetch_add(1, std::memory_order_relaxed);
        meters[slot].store(ok ? value : 0.0f, std::memory_order_relaxed);
        meter_mask.store(meter_mask.load(std::memory_order_relaxed) | (1u << slot),
                         std::memory_order_relaxed);
    }
    // A curve longer than kMaxCurvePoints is cut, never written past its end.
    void set_curve(uint32_t index, const float* points, uint32_t count) noexcept SONORA_RT {
        if (index >= kMaxTelemetryCurves) return;
        if (count > kMaxCurvePoints) count = kMaxCurvePoints;
        for (uint32_t i = 0; i < count; ++i) {
            const float v = points[i];
            const bool ok = std::isfinite(v);
            if (!ok) rejected.fetch_add(1, std::memory_order_relaxed);
            curves[index][i].store(ok ? v : 0.0f, std::memory_order_relaxed);
        }
        curve_points[index].store(count, std::memory_order_relaxed);
    }

    // --- reader (the engine's control thread) ------------------------------
    // True with `out` filled from one consistent frame. False: never written,
    // a count past the cap (only a scribbling plugin writes one), or every
    // attempt overlapped a write; the caller keeps its last frame. A meter
    // that is not finite loses its mask bit, a curve point that is not
    // finite reads 0.
    bool read(TelemetrySnapshot& out) const noexcept {
        for (int attempt = 0; attempt < kTelemetryReadAttempts; ++attempt) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 == 0) return false;
            if ((s1 & 1u) != 0) continue;
            TelemetrySnapshot t;
            t.sequence = s1 / 2;
            t.meter_mask = meter_mask.load(std::memory_order_relaxed);
            bool bad = false;
            for (uint32_t i = 0; i < kMaxTelemetryMeters; ++i) {
                t.meters[i] = meters[i].load(std::memory_order_relaxed);
            }
            for (uint32_t c = 0; c < kMaxTelemetryCurves; ++c) {
                const uint32_t n = curve_points[c].load(std::memory_order_relaxed);
                if (n > kMaxCurvePoints) {
                    bad = true;
                    break;
                }
                t.curve_points[c] = n;
                for (uint32_t i = 0; i < n; ++i) {
                    t.curves[c][i] = curves[c][i].load(std::memory_order_relaxed);
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) != s1) continue;
            if (bad) return false;
            t.meter_mask &= (1u << kMaxTelemetryMeters) - 1;
            for (uint32_t i = 0; i < kMaxTelemetryMeters; ++i) {
                if (!std::isfinite(t.meters[i])) t.meter_mask &= ~(1u << i);
            }
            for (uint32_t c = 0; c < kMaxTelemetryCurves; ++c) {
                for (uint32_t i = 0; i < t.curve_points[c]; ++i) {
                    if (!std::isfinite(t.curves[c][i])) t.curves[c][i] = 0.0f;
                }
            }
            out = t;
            return true;
        }
        return false;
    }
};

struct TelemetryHeader {
    uint32_t magic;
    uint32_t version;       // kTelemetryVersion
    uint32_t header_bytes;  // sizeof(TelemetryHeader)
    uint32_t block_bytes;   // sizeof(TelemetryBlock)
    uint32_t instance_count;
    uint32_t reserved[3];
};

static_assert(std::atomic<float>::is_always_lock_free, "telemetry floats must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "telemetry words must be lock-free");
static_assert(sizeof(std::atomic<float>) == sizeof(float), "telemetry floats are plain floats");
static_assert(sizeof(TelemetryHeader) == 32, "TelemetryHeader is 32 bytes");
static_assert(alignof(TelemetryBlock) == kCacheLine, "a block starts on its own cache line");
static_assert(kMaxTelemetryMeters <= 32, "the meter mask is one word");

constexpr std::size_t telemetry_blocks_offset() {
    return align_up(sizeof(TelemetryHeader), kCacheLine);
}
constexpr std::size_t telemetry_block_offset(uint32_t instance) {
    return telemetry_blocks_offset() + std::size_t(instance) * sizeof(TelemetryBlock);
}
// A block for each of kMaxInstances, so the region's size is a constant and
// a host never needs to be told it.
constexpr std::size_t telemetry_region_size() { return telemetry_block_offset(kMaxInstances); }

// Control thread, once, on memory of telemetry_region_size() bytes aligned
// to kCacheLine. An atomic in raw memory is not alive until it is
// constructed, so every block is.
inline void telemetry_init(void* base, uint32_t instances) {
    auto* h = static_cast<TelemetryHeader*>(base);
    h->magic = kTelemetryMagic;
    h->version = kTelemetryVersion;
    h->header_bytes = sizeof(TelemetryHeader);
    h->block_bytes = sizeof(TelemetryBlock);
    h->instance_count = instances > kMaxInstances ? kMaxInstances : instances;
    h->reserved[0] = h->reserved[1] = h->reserved[2] = 0;
    for (uint32_t b = 0; b < kMaxInstances; ++b) {
        auto* t = new (static_cast<char*>(base) + telemetry_block_offset(b)) TelemetryBlock;
        t->seq.store(0, std::memory_order_relaxed);
        t->meter_mask.store(0, std::memory_order_relaxed);
        t->rejected.store(0, std::memory_order_relaxed);
        for (auto& c : t->curve_points) c.store(0, std::memory_order_relaxed);
        for (auto& m : t->meters) m.store(0.0f, std::memory_order_relaxed);
        for (auto& c : t->curves) {
            for (auto& p : c) p.store(0.0f, std::memory_order_relaxed);
        }
    }
}

// Does a mapped region look like ours? Reads the header into private values;
// the caller computes block addresses from telemetry_block(), never from the
// header's words.
inline bool telemetry_header_valid(const void* base, uint32_t& instances) {
    const auto* h = static_cast<const TelemetryHeader*>(base);
    if (h->magic != kTelemetryMagic || h->version != kTelemetryVersion ||
        h->header_bytes != sizeof(TelemetryHeader) || h->block_bytes != sizeof(TelemetryBlock) ||
        h->instance_count > kMaxInstances) {
        return false;
    }
    instances = h->instance_count;
    return true;
}

inline const TelemetryBlock* telemetry_block(const void* base, uint32_t instance) {
    return instance >= kMaxInstances
               ? nullptr
               : reinterpret_cast<const TelemetryBlock*>(static_cast<const char*>(base) +
                                                         telemetry_block_offset(instance));
}
inline TelemetryBlock* telemetry_block(void* base, uint32_t instance) {
    return instance >= kMaxInstances
               ? nullptr
               : reinterpret_cast<TelemetryBlock*>(static_cast<char*>(base) +
                                                   telemetry_block_offset(instance));
}

}  // namespace sonora::plugin
