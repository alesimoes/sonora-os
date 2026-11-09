// SPDX-License-Identifier: GPL-3.0-or-later
// Which clock the interface runs from, and whether it is locked (ADR-0125).
// The sample clock is hardware: software can only pick the source on the card
// and observe lock. Control thread only; nothing here runs on the audio thread.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sonora::clock {

enum class ClockSource : std::uint8_t { Internal, Adat, Spdif, WordClock };

constexpr std::size_t kClockSourceCount = 4;

// Unknown means the card does not report validity at all, which is not the
// same as Unlocked: ClockGuard asks the user to confirm Unknown once per device.
enum class ClockLock : std::uint8_t { Locked, Unlocked, Unknown };

constexpr const char* to_string(ClockSource source) {
    switch (source) {
        case ClockSource::Internal: return "internal";
        case ClockSource::Adat: return "ADAT";
        case ClockSource::Spdif: return "S/PDIF";
        case ClockSource::WordClock: return "word clock";
    }
    return "?";
}

constexpr std::size_t index_of(ClockSource source) {
    return static_cast<std::size_t>(source);
}

struct ClockCaps {
    std::array<bool, kClockSourceCount> offered{};
    // False when the card exposes no validity control: lock() is then Unknown.
    bool reports_lock = false;

    bool offers(ClockSource source) const { return offered[index_of(source)]; }
    void offer(ClockSource source) { offered[index_of(source)] = true; }
};

// A refusal carries its reason so it can be shown to the operator as is.
struct ClockResult {
    bool ok = false;
    std::string reason;

    static ClockResult success() { return {true, {}}; }
    static ClockResult failure(std::string why) { return {false, std::move(why)}; }
};

// The one interface the rest of the program sees; injected, never a global.
// AlsaClockControl implements it over a ClockMixer; tests use FakeClockControl.
class ClockSourceControl {
public:
    virtual ~ClockSourceControl() = default;

    virtual ClockCaps caps() const = 0;
    virtual ClockResult select(ClockSource source) = 0;
    virtual ClockSource selected() const = 0;
    virtual ClockLock lock() const = 0;
};

// Defined in src/clock_version.cpp so sonora_clock is never an empty archive.
const char* clock_module_name();

}  // namespace sonora::clock
