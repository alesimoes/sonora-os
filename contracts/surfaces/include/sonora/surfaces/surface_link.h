// SPDX-License-Identifier: GPL-3.0-or-later
// The control surface boundary (ADR-0007 4.4, ADR-0040). Plain data and two
// RingBuffers, no protocol: HUI and MCU are separate items that sit on the
// surface thread and speak only these types. Header-only, OS-free (ADR-0008).
//
// The surface thread never calls the engine. It pushes SurfaceEvents; the
// engine side drains them at a block boundary and turns them into the same
// commands and parameter changes the UI sends (4.1). Feedback goes back the
// same way. A full ring drops and counts; nobody waits on it (CLAUDE.md).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "sonora/ringbuffer.h"

namespace sonora::surfaces {

constexpr std::uint32_t kSurfaceContractVersion = 1;
constexpr std::uint8_t kMaxStrips = 32;
constexpr std::size_t kScribbleChars = 7;  // the MCU strip display; HUI uses 4
constexpr std::size_t kLinkCapacity = 256;

enum class EventKind : std::uint8_t {
    Fader,      // value 0..1, normalized; flag = touched (touch brackets a gesture)
    Pan,        // value -1..1
    Mute,       // flag
    Solo,       // flag
    Arm,        // flag
    Transport,  // transport field
    Select,     // flag
};

enum class TransportCmd : std::uint8_t { None, Play, Stop, Record, Rewind, Forward, Loop };

// The surface says what moved by its own strip index, not by track id: the
// engine side maps it through the bank offset, so banking never reaches the
// protocol code on the surface thread.
struct SurfaceEvent {
    // No default member initialisers: the ring clears its storage with memset,
    // so the type stays trivial. Write `SurfaceEvent e{};` to zero one.
    EventKind kind;
    std::uint8_t strip;
    bool flag;
    TransportCmd transport;
    float value;
};

enum class FeedbackKind : std::uint8_t {
    Fader,      // motor fader target
    Pan,
    Mute,
    Solo,
    Arm,
    Meter,      // value 0..1 peak at the surface tick, latest wins
    Scribble,   // text, space padded, not NUL terminated
    Transport,  // flag = lit; transport = which light
};

struct SurfaceFeedback {
    FeedbackKind kind;
    std::uint8_t strip;
    bool flag;
    TransportCmd transport;
    float value;
    char text[kScribbleChars];  // set_text() pads; zero with `SurfaceFeedback f{};`

    void set_text(const char* s) {
        std::size_t i = 0;
        for (; i < kScribbleChars && s[i] != '\0'; ++i) text[i] = s[i];
        for (; i < kScribbleChars; ++i) text[i] = ' ';
    }
};

static_assert(std::is_trivially_copyable<SurfaceEvent>::value, "rings copy bytes");
static_assert(std::is_trivially_copyable<SurfaceFeedback>::value, "rings copy bytes");

// One surface's pair of rings and its drop counters. Single producer and
// single consumer on each ring: events surface -> engine, feedback engine ->
// surface.
class SurfaceLink {
public:
    explicit SurfaceLink(std::size_t capacity = kLinkCapacity)
        : events_(capacity), feedback_(capacity) {}

    // Surface thread.
    bool push_event(const SurfaceEvent& e) {
        if (events_.write(&e, 1) == 1) return true;
        events_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool pop_feedback(SurfaceFeedback& f) { return feedback_.read(&f, 1) == 1; }

    // Engine side, real-time safe: no allocation, no lock.
    bool pop_event(SurfaceEvent& e) { return events_.read(&e, 1) == 1; }
    bool push_feedback(const SurfaceFeedback& f) {
        if (feedback_.write(&f, 1) == 1) return true;
        feedback_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint64_t events_dropped() const { return events_dropped_.load(std::memory_order_relaxed); }
    std::uint64_t feedback_dropped() const {
        return feedback_dropped_.load(std::memory_order_relaxed);
    }

private:
    RingBuffer<SurfaceEvent> events_;
    RingBuffer<SurfaceFeedback> feedback_;
    std::atomic<std::uint64_t> events_dropped_{0};
    std::atomic<std::uint64_t> feedback_dropped_{0};
};

// Implemented by HUI and MCU later, on the surface thread. It decodes MIDI
// into events and encodes feedback to MIDI; it owns no engine state.
class SurfaceProtocol {
public:
    virtual ~SurfaceProtocol() = default;
    virtual void on_midi(const std::uint8_t* bytes, std::size_t n, SurfaceLink&) = 0;
    virtual void render(SurfaceLink&) = 0;  // drain feedback, diff, write MIDI
};

}  // namespace sonora::surfaces
