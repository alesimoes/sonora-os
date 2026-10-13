// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Turns a live MIDI 1.0 byte stream into whole messages: running status
// expanded, real-time bytes interleaved anywhere, SysEx collected.
//
// For the thread that reads a MIDI port, not the audio thread: the SysEx
// buffer is a vector. The port reader parses here and hands short
// messages to the audio thread as raw bytes through a ring.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "sonora/midi/event.h"

namespace sonora::midi {

struct WireMessage {
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size = 0;  // 1..3, status included
};

class StreamParser {
public:
    enum class Result : std::uint8_t {
        None,     // the byte was consumed; nothing complete yet
        Message,  // message() holds a channel, system common or real-time message
        SysEx,    // sysex() holds a whole SysEx, 0xF0 through 0xF7
    };

    explicit StreamParser(std::size_t max_sysex_bytes = 64 * 1024)
        : max_sysex_(max_sysex_bytes) {}

    Result feed(std::uint8_t byte);

    const WireMessage& message() const { return message_; }
    const std::vector<std::uint8_t>& sysex() const { return sysex_; }

    // Bytes dropped: data with no status in effect, a SysEx cut off by a
    // status byte or over the size limit. A noisy cable shows up here
    // instead of as wrong notes.
    std::size_t errors() const { return errors_; }

private:
    void abandon_sysex();

    std::size_t max_sysex_;
    // Status of the message being collected. For a channel message it
    // stays after the message completes: that is running status.
    std::uint8_t pending_ = 0;
    std::uint8_t needed_ = 0;    // data bytes it needs
    std::uint8_t have_ = 0;
    std::array<std::uint8_t, 2> data_{};
    bool in_sysex_ = false;
    bool sysex_overflow_ = false;
    std::vector<std::uint8_t> sysex_;
    WireMessage message_;
    std::size_t errors_ = 0;
};

// A channel message as an Event at the given tick. Empty for system
// messages, which have no place in a track.
std::optional<Event> to_event(const WireMessage& message, Tick tick);

}  // namespace sonora::midi
