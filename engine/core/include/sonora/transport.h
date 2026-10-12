// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace sonora {

enum class TransportState : int {
    Stopped = 0,
    Rolling = 1,
    Recording = 2,
};

// Shared transport clock. The callback is the only writer of the playhead, so
// readers see a value that is at most one block stale.
class Transport {
public:
    explicit Transport(uint32_t sample_rate) : sample_rate_(sample_rate) {}

    uint32_t sample_rate() const { return sample_rate_; }

    TransportState state() const { return state_.load(std::memory_order_acquire); }
    void set_state(TransportState s) { state_.store(s, std::memory_order_release); }

    uint64_t playhead() const { return playhead_.load(std::memory_order_relaxed); }
    void set_playhead(uint64_t p) { playhead_.store(p, std::memory_order_relaxed); }

    // Called from the callback once per block.
    void advance(uint32_t frames) {
        playhead_.fetch_add(frames, std::memory_order_relaxed);
    }

    double seconds() const {
        return static_cast<double>(playhead()) / sample_rate_;
    }

private:
    const uint32_t sample_rate_;
    std::atomic<TransportState> state_{TransportState::Stopped};
    std::atomic<uint64_t> playhead_{0};
};

}  // namespace sonora
