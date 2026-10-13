// SPDX-License-Identifier: GPL-3.0-or-later
//
// The state publisher (ADR-0007 §2, §4.1; 2026-10-12 update, item 3).
//
// A thread at normal priority that copies what the audio thread already
// writes -- Transport's state and playhead, each Track's arm, mute, gain,
// peak and overrun atomics -- into the shared state block, on a fixed
// timer. The audio thread is not touched: it writes no new atomic and
// learns of no client. The publisher is the only caller of
// Track::take_peak(), so the exchange-to-clear loses no peak between
// publishes, and readers keep cursors of their own in the block.
//
// Paced, never "on change": an unpaced republish loop starved a 60 Hz
// reader to a p99 of 41 ms in the measurement. If a publish runs late the
// next one is due a period after it, not at once; it never catches up in
// a burst.
//
// What only the control thread knows (the take number, input connections,
// health) it hands over through the setters below, which are atomics.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "sonora/api/state_block.h"

namespace sonora {

class Session;
namespace backend {
class AudioBackend;
}

class StatePublisher {
public:
    // `audio` may be null (a session with no backend, in a test). `writer`
    // must be initialised for at least the session's track count.
    StatePublisher(Session& session, const backend::AudioBackend* audio,
                   api::StateWriter& writer, uint64_t period_ns);
    ~StatePublisher();
    StatePublisher(const StatePublisher&) = delete;
    StatePublisher& operator=(const StatePublisher&) = delete;

    void start();
    // Joins the thread. Idempotent. After it returns, nothing more is
    // published, so the heartbeat stands still.
    void stop();
    bool running() const { return thread_.joinable(); }

    // One publish, on the caller's thread. The thread calls nothing else.
    // Never call it while the thread runs: the block has one writer.
    void publish_once();

    // --- from the control thread ---
    void set_take(int32_t take) { take_.store(take, std::memory_order_relaxed); }
    void set_health(uint32_t flag, bool on);
    void set_inputs_connected(uint32_t count) {
        inputs_connected_.store(count, std::memory_order_relaxed);
    }
    void set_input_connected(uint32_t track, bool connected);

    uint64_t period_ns() const { return period_ns_; }
    uint64_t publishes() const { return publishes_.load(std::memory_order_relaxed); }

private:
    void run();

    Session& session_;
    const backend::AudioBackend* audio_;
    api::StateWriter& writer_;
    const uint64_t period_ns_;

    std::atomic<int32_t> take_{0};
    std::atomic<uint32_t> health_{0};
    std::atomic<uint32_t> inputs_connected_{0};
    std::unique_ptr<std::atomic<bool>[]> input_connected_;

    // The publisher thread's own; sized once, so a publish never allocates.
    std::vector<api::TrackFields> tracks_;
    std::vector<float> peaks_;
    std::vector<uint32_t> clips_;

    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> publishes_{0};
    std::thread thread_;
};

}  // namespace sonora
