// SPDX-License-Identifier: GPL-3.0-or-later
// A scripted device: the test feeds bytes the "hardware" sends and reads
// back every message the host wrote.
#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "sonora/surfaces/midi_port.h"

namespace sonora::surfaces {

class FakeMidiPort : public MidiPort {
public:
    void feed(std::initializer_list<std::uint8_t> bytes) { in_.insert(in_.end(), bytes); }
    std::size_t read(std::uint8_t* buf, std::size_t cap) override {
        const std::size_t n = std::min(cap, in_.size());
        std::copy(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n), buf);
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    bool write(const std::uint8_t* bytes, std::size_t n) override {
        if (reject_) return false;
        sent.emplace_back(bytes, bytes + n);
        return true;
    }
    void reject_writes(bool r) { reject_ = r; }

    std::vector<std::vector<std::uint8_t>> sent;

    // Messages written since the last call, then forgotten.
    std::vector<std::vector<std::uint8_t>> take() {
        std::vector<std::vector<std::uint8_t>> out;
        out.swap(sent);
        return out;
    }

private:
    std::vector<std::uint8_t> in_;
    bool reject_ = false;
};

}  // namespace sonora::surfaces
