// SPDX-License-Identifier: GPL-3.0-or-later
// The seam between clock control and the card's mixer (ADR-0125 §3).
// snd-usb-audio exposes UAC2 clock units as mixer controls ("Clock Selector",
// "Clock Source N Validity") only when the descriptors say so; what the
// UMC1820 exposes is unverified. Keeping libasound behind this seam means that
// if the card exposes something else, only the adapter changes.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "sonora/clock/clock_source.h"

namespace sonora::clock {

enum class MixerElementKind { Enum, Bool, Other };

struct MixerElement {
    std::string name;
    MixerElementKind kind = MixerElementKind::Other;
    // Item names of an Enum element, in the card's index order.
    std::vector<std::string> items;
};

class ClockMixer {
public:
    virtual ~ClockMixer() = default;

    virtual std::vector<MixerElement> elements() const = 0;

    // An absent element, or one of another kind, reads as nullopt and a
    // write to it returns false: the caller turns that into a reason.
    virtual std::optional<std::size_t> read_enum(const std::string& name) const = 0;
    virtual bool write_enum(const std::string& name, std::size_t index) = 0;
    virtual std::optional<bool> read_bool(const std::string& name) const = 0;
    virtual bool write_bool(const std::string& name, bool value) = 0;
};

// ClockSourceControl over a ClockMixer. Which element names mean which source
// is one table in src/alsa_clock_control.cpp, not strings scattered around.
class AlsaClockControl final : public ClockSourceControl {
public:
    explicit AlsaClockControl(ClockMixer& mixer);

    ClockCaps caps() const override;
    ClockResult select(ClockSource source) override;
    ClockSource selected() const override;
    ClockLock lock() const override;

private:
    ClockMixer& mixer_;
};

}  // namespace sonora::clock
