// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/stream_parser.h"

namespace sonora::midi {

namespace {

std::uint8_t data_bytes_for(std::uint8_t st) {
    switch (st & 0xF0) {
        case status::program_change:
        case status::channel_aftertouch:
            return 1;
        case 0xF0:
            break;
        default:
            return 2;
    }
    switch (st) {
        case 0xF1:  // MTC quarter frame
        case 0xF3:  // song select
            return 1;
        case 0xF2:  // song position pointer
            return 2;
        default:
            return 0;
    }
}

}  // namespace

void StreamParser::abandon_sysex() {
    // An overflowing SysEx was counted when it overflowed.
    if (!sysex_overflow_) ++errors_;
    in_sysex_ = false;
    sysex_overflow_ = false;
    sysex_.clear();
}

StreamParser::Result StreamParser::feed(std::uint8_t byte) {
    // Real-time messages may arrive between any two bytes, even inside a
    // SysEx or between the data bytes of a note, and disturb nothing.
    if (byte >= 0xF8) {
        message_ = WireMessage{{byte, 0, 0}, 1};
        return Result::Message;
    }

    if (byte < 0x80) {
        if (in_sysex_) {
            // One byte stays free for the closing 0xF7.
            if (sysex_.size() + 1 >= max_sysex_) {
                if (!sysex_overflow_) ++errors_;
                sysex_overflow_ = true;
            } else {
                sysex_.push_back(byte);
            }
            return Result::None;
        }
        if (pending_ == 0) {
            ++errors_;
            return Result::None;
        }
        data_[have_++] = byte;
        if (have_ < needed_) return Result::None;

        message_ = WireMessage{{pending_, data_[0], needed_ == 2 ? data_[1] : std::uint8_t{0}},
                               static_cast<std::uint8_t>(needed_ + 1)};
        have_ = 0;
        // Running status applies to channel messages only; system common
        // messages clear it.
        if (pending_ >= 0xF0) pending_ = 0;
        return Result::Message;
    }

    // A status byte. Any but 0xF7 ends a SysEx without its 0xF7, which
    // MIDI 1.0 allows as an end; we drop the message rather than guess
    // that it was complete.
    if (in_sysex_) {
        if (byte == 0xF7) {
            const bool overflowed = sysex_overflow_;
            in_sysex_ = false;
            sysex_overflow_ = false;
            if (overflowed) {
                sysex_.clear();
                return Result::None;
            }
            sysex_.push_back(byte);
            return Result::SysEx;
        }
        abandon_sysex();
    }

    if (have_ > 0) ++errors_;  // a message cut short by this status byte
    have_ = 0;

    if (byte < 0xF0) {
        pending_ = byte;
        needed_ = data_bytes_for(byte);
        return Result::None;
    }

    // System common: clears running status.
    pending_ = 0;
    if (byte == 0xF0) {
        in_sysex_ = true;
        sysex_overflow_ = false;
        sysex_.clear();
        sysex_.push_back(byte);
        return Result::None;
    }
    const std::uint8_t needed = data_bytes_for(byte);
    if (needed > 0) {
        pending_ = byte;
        needed_ = needed;
        return Result::None;
    }
    if (byte == 0xF6) {  // tune request, the one status-only system common
        message_ = WireMessage{{byte, 0, 0}, 1};
        return Result::Message;
    }
    // 0xF4, 0xF5 are undefined, and 0xF7 outside a SysEx ends nothing.
    ++errors_;
    return Result::None;
}

std::optional<Event> to_event(const WireMessage& message, Tick tick) {
    const std::uint8_t st = message.bytes[0];
    if (st < 0x80 || st >= 0xF0 || message.size < 2) return std::nullopt;
    Event e;
    e.tick = tick;
    e.status = st;
    e.data1 = message.bytes[1];
    e.data2 = message.size == 3 ? message.bytes[2] : 0;
    return e;
}

}  // namespace sonora::midi
