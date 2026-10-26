// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "sonora/process_callback.h"
#include "sonora/ringbuffer.h"
#include "sonora/rt.h"

namespace sonora {

// The round trip from a sample the callback writes to the sample the
// callback reads back, measured through a cable from an output to an input
// (ADR-0019 §1). Per ping it plays `window` frames of silence, which give
// the noise floor and let the last ping's tail die out, then one impulse,
// and listens `window` frames for the input's largest absolute sample: its
// offset from the impulse is that ping's round trip. Gain and polarity of
// the loop do not move it, and on a converter's symmetric filter it is the
// filter's centre, the delay a recording is shifted by.
//
// Requests and results cross by ring; the audio thread never waits. The
// session runs it at the end of every callback, after the mix, so while it
// runs it overwrites its output channel and nothing else.
class LatencyProbe {
public:
    static constexpr uint32_t kMaxPings = 16;
    // An arrival quieter than -60 dBFS, or less than 12 dB over the floor,
    // is not one.
    static constexpr float kMinPeak = 0.001f;
    static constexpr float kMinSnr = 4.0f;
    // The pings may disagree by this much, a converter's rounding of a
    // fractional delay.
    static constexpr uint32_t kMaxSpread = 1;

    struct Settings {
        uint32_t output = 0;  // AudioBlock::outputs index
        uint32_t input = 0;   // AudioBlock::inputs index
        uint32_t pings = 3;
        // The longest round trip looked for. 0: a quarter of a second.
        uint32_t window = 0;
        float level = 0.1f;   // -20 dBFS
    };

    enum class Outcome : uint32_t {
        Measured = 0,
        NoSignal = 1,       // a ping heard nothing that counts as its arrival
        Unstable = 2,       // the pings disagree by more than kMaxSpread
        Discontinuity = 3,  // the backend's frames jumped more often than restarts allow
        NoPort = 4,         // the output or the input is not in the block
        Cancelled = 5,
    };

    // Trivial, so the ring holds it as bytes.
    struct Result {
        Outcome outcome;
        uint32_t frames;    // the median ping, when measured
        uint32_t pings;     // pings completed
        uint32_t spread;    // largest less smallest ping
        float peak;         // the quietest ping's arrival
        float noise;        // the loudest floor heard before an impulse
        uint32_t restarts;  // pings restarted by a discontinuity
    };

    explicit LatencyProbe(uint32_t sample_rate);

    // --- control thread ---
    // False, with `error`, while one runs or for settings out of range.
    bool start(const Settings& settings, std::string* error = nullptr);
    // Asks the audio thread to stop; the result says Cancelled.
    void cancel();
    // True once per measurement, when its result has arrived.
    bool poll(Result& out);
    bool running() const { return running_; }
    // Pings completed by the measurement running now or last, for showing
    // progress; any thread.
    uint32_t pings_heard() const { return pings_heard_.load(std::memory_order_relaxed); }
    // In words, with the figures, for the operator. Allocates.
    static std::string describe(const Result& r, uint32_t sample_rate);

    // --- audio thread ---
    void process(const backend::AudioBlock& block) noexcept SONORA_RT;

private:
    // Trivial, as the ring wants: Settings' defaults would make it not.
    struct Request {
        uint32_t cancel;
        uint32_t output;
        uint32_t input;
        uint32_t pings;
        uint32_t window;
        float level;
    };
    // Settle: silence after a discontinuity, measuring nothing, so an
    // impulse still in the loop arrives before the next floor is taken.
    enum class Phase : uint32_t { Idle, Settle, Quiet, Listen };

    Result summary(Outcome outcome) const noexcept SONORA_RT;
    void finish(const Result& r) noexcept SONORA_RT;
    void evaluate() noexcept SONORA_RT;

    const uint32_t sample_rate_;
    std::unique_ptr<RingBuffer<Request>> requests_;
    std::unique_ptr<RingBuffer<Result>> results_;
    bool running_ = false;  // control thread
    // ping_, published: the audio thread stores, nothing waits on it.
    std::atomic<uint32_t> pings_heard_{0};

    // Audio thread.
    Settings run_{};
    Phase phase_ = Phase::Idle;
    uint32_t pos_ = 0;      // frames into the phase
    uint32_t ping_ = 0;     // pings completed
    uint32_t restarts_ = 0;
    bool have_time_ = false;
    uint64_t next_time_ = 0;
    float floor_ = 0.0f;    // this ping's floor
    float noise_ = 0.0f;
    float best_ = 0.0f;
    uint32_t best_at_ = 0;
    uint32_t lag_[kMaxPings] = {};
    float peak_[kMaxPings] = {};
};

}  // namespace sonora
