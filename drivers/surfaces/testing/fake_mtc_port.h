// SPDX-License-Identifier: GPL-3.0-or-later
// A scripted MTC master or slave (ADR-0122): the test queues the bytes the
// "device" sends and caps how many one read() returns, so a message can be
// split at any byte, as a USB MIDI or raw MIDI node splits it. Every write
// the host makes is kept.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "sonora/surfaces/midi_port.h"

namespace sonora::surfaces {

class FakeMtcPort : public MidiPort {
public:
    void feed(std::initializer_list<std::uint8_t> bytes) { in_.insert(in_.end(), bytes); }
    void feed(const std::uint8_t* bytes, std::size_t n) { in_.insert(in_.end(), bytes, bytes + n); }
    // At most `n` bytes per read(); 0 means no cap.
    void set_read_chunk(std::size_t n) { chunk_ = n; }
    std::size_t pending() const { return in_.size(); }

    std::size_t read(std::uint8_t* buf, std::size_t cap) override {
        std::size_t n = std::min(cap, in_.size());
        if (chunk_ != 0) n = std::min(n, chunk_);
        std::copy(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n), buf);
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n));
        ++reads;
        return n;
    }
    bool write(const std::uint8_t* bytes, std::size_t n) override {
        if (reject_) return false;
        sent.emplace_back(bytes, bytes + n);
        return true;
    }
    void reject_writes(bool r) { reject_ = r; }

    std::vector<std::vector<std::uint8_t>> sent;
    std::size_t reads = 0;

private:
    std::vector<std::uint8_t> in_;
    std::size_t chunk_ = 0;
    bool reject_ = false;
};

}  // namespace sonora::surfaces
