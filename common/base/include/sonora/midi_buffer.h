// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace sonora {

// One block's worth of MIDI for the audio thread, ordered by sample offset.
//
// It lives in common/base, not common/midi, because ADR-0007 §2 lets only
// common/base contribute code to the audio thread among the common modules.
// It knows nothing about MIDI semantics: a message is 1 or more raw bytes
// (a channel message, or a whole SysEx) at a sample offset in the block.
//
// Both capacities are fixed at construction, which is the only place it
// allocates. A push that does not fit is dropped and counted, never waited on.
class MidiBuffer {
public:
    struct Message {
        std::uint32_t offset;       // sample offset within the block
        std::uint32_t size;         // bytes in data
        const std::uint8_t* data;   // valid until the next clear()
    };

    MidiBuffer(std::size_t max_messages, std::size_t max_bytes)
        : max_messages_(max_messages),
          max_bytes_(max_bytes),
          entries_(new Entry[max_messages > 0 ? max_messages : 1]),
          bytes_(new std::uint8_t[max_bytes > 0 ? max_bytes : 1]) {}

    MidiBuffer(const MidiBuffer&) = delete;
    MidiBuffer& operator=(const MidiBuffer&) = delete;

    // --- real-time safe: callable from the audio callback ---

    // Inserts after every message already at the same offset, so messages
    // pushed in order at one offset keep that order (a CC before the note it
    // shapes, say). Pushing in offset order costs O(1); out of order, the
    // insertion shifts entries, bounded by the capacity.
    bool push(std::uint32_t offset, const std::uint8_t* data,
              std::size_t size) noexcept {
        if (size == 0 || count_ == max_messages_ ||
            size > max_bytes_ - used_bytes_) {
            ++dropped_;
            return false;
        }
        std::memcpy(bytes_.get() + used_bytes_, data, size);
        const Entry entry{offset, static_cast<std::uint32_t>(size),
                          static_cast<std::uint32_t>(used_bytes_)};
        used_bytes_ += size;

        std::size_t at = count_;
        while (at > 0 && entries_[at - 1].offset > offset) --at;
        if (at < count_) {
            std::memmove(&entries_[at + 1], &entries_[at],
                         (count_ - at) * sizeof(Entry));
        }
        entries_[at] = entry;
        ++count_;
        return true;
    }

    bool push(std::uint32_t offset, std::uint8_t status, std::uint8_t data1,
              std::uint8_t data2) noexcept {
        const std::uint8_t bytes[3] = {status, data1, data2};
        return push(offset, bytes, short_message_size(status));
    }

    void clear() noexcept {
        count_ = 0;
        used_bytes_ = 0;
    }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    std::size_t max_messages() const noexcept { return max_messages_; }
    std::size_t max_bytes() const noexcept { return max_bytes_; }

    // Survives clear(), so the owner can report losses accumulated over many
    // blocks. Not atomic: the owner publishes it like any other overrun count.
    std::uint64_t dropped() const noexcept { return dropped_; }

    Message operator[](std::size_t i) const noexcept {
        const Entry& e = entries_[i];
        return Message{e.offset, e.size, bytes_.get() + e.byte_pos};
    }

    // Length of a channel or system message from its status byte; SysEx
    // (0xF0) has no fixed length and returns 1, its status byte alone.
    static constexpr std::size_t short_message_size(std::uint8_t status) noexcept {
        switch (status & 0xF0) {
            case 0xC0:
            case 0xD0:
                return 2;
            case 0x80:
            case 0x90:
            case 0xA0:
            case 0xB0:
            case 0xE0:
                return 3;
            default:
                break;
        }
        switch (status) {
            case 0xF1:
            case 0xF3:
                return 2;
            case 0xF2:
                return 3;
            default:
                return 1;
        }
    }

private:
    // Entries index into bytes_, so reordering moves 12 bytes, never payload.
    struct Entry {
        std::uint32_t offset;
        std::uint32_t size;
        std::uint32_t byte_pos;
    };

    const std::size_t max_messages_;
    const std::size_t max_bytes_;
    std::unique_ptr<Entry[]> entries_;
    std::unique_ptr<std::uint8_t[]> bytes_;
    std::size_t count_ = 0;
    std::size_t used_bytes_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace sonora
