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

    TransportState state() const { return state_of(word_.load(std::memory_order_acquire)); }
    // Stopped and Rolling record nothing; Recording here is take 0, which no
    // file is opened for (tests drive the capture ring with it).
    void set_state(TransportState s) { word_.store(word(s, 0), std::memory_order_seq_cst); }
    // Recording into take `take`. One store with the state, so the audio
    // thread never reads one take's state with another's number.
    void set_recording(uint32_t take) {
        word_.store(word(TransportState::Recording, take), std::memory_order_seq_cst);
    }
    // The take being recorded, 0 when none is.
    uint32_t take() const { return take_of(word_.load(std::memory_order_acquire)); }

    // The audio thread's read of both, once per block. Sequentially
    // consistent, because Session::stop() orders its store of Stopped
    // against the callback's own flag (Session::in_callback_) the same way.
    struct Word {
        TransportState state;
        uint32_t take;
    };
    Word load_word() const {
        const uint64_t w = word_.load(std::memory_order_seq_cst);
        return {state_of(w), take_of(w)};
    }

    uint64_t playhead() const { return playhead_.load(std::memory_order_relaxed); }
    void set_playhead(uint64_t p) { playhead_.store(p, std::memory_order_relaxed); }

    // Called from the callback once per block.
    void advance(uint32_t frames) {
        playhead_.fetch_add(frames, std::memory_order_relaxed);
    }

    // How many locates the callback has applied. It applies one at a block
    // start, after setting the playhead, so a playhead read after this count
    // is at or past the locate it reports.
    uint64_t locates_applied() const { return locates_.load(std::memory_order_acquire); }
    // Audio thread only, after set_playhead().
    void note_locate() { locates_.fetch_add(1, std::memory_order_release); }

    double seconds() const {
        return static_cast<double>(playhead()) / sample_rate_;
    }

private:
    static uint64_t word(TransportState s, uint32_t take) {
        return (uint64_t{take} << 8) | static_cast<uint64_t>(s);
    }
    static TransportState state_of(uint64_t w) { return static_cast<TransportState>(w & 0xFF); }
    static uint32_t take_of(uint64_t w) { return static_cast<uint32_t>(w >> 8); }

    const uint32_t sample_rate_;
    std::atomic<uint64_t> word_{0};
    std::atomic<uint64_t> playhead_{0};
    std::atomic<uint64_t> locates_{0};
};

// The callback loads the state and advances the playhead every period; a
// lock behind either would be a lock on the audio thread.
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "written by the audio thread every period");

}  // namespace sonora
