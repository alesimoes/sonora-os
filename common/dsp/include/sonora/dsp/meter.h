// SPDX-License-Identifier: GPL-3.0-or-later
//
// The track and master meter (ADR-0011 §1.3): the one piece of common/dsp
// the engine's callback may call. Peak, true-peak and BS.1770 loudness for
// one track or bus, written by the audio thread and read lock-free by any
// other thread.
//
// - Sample peak, with two readings. take_peak() is the largest |x| since
//   the last take, so a reader at any rate misses no peak. display_peak()
//   is the same peak with programme-meter ballistics: instant rise, and a
//   fall of 20 dB in 1.7 s, the return time of IEC 60268-10's Type I PPM
//   (the default; the rate is a prepare() argument). The fall is applied per
//   sample, so the reading does not depend on how often it is read.
// - True-peak per BS.1770-5 Annex 2 (true_peak.h, the meter's 32 taps per
//   phase, which passes every EBU Tech 3341 true-peak case, run in block
//   mode: 3.5x cheaper than per sample, measured in tests/test_meter.cpp). take_true_peak()
//   since the last take, true_peak_max_dbtp() since reset. It lags the
//   sample peak by the interpolator's delay, 16 samples.
// - Loudness per BS.1770-5 and EBU Tech 3341/3342 (loudness.h): momentary
//   and short-term every 10 ms, integrated and LRA every 100 ms, computed on
//   the audio thread in O(log bins) and published, so a reader never touches
//   the histograms the audio thread is writing.
//
// Real-time: prepare() allocates everything; process() never allocates,
// locks or blocks, holds FTZ/DAZ for its duration (denormals.h), and keeps
// every reading finite whatever the input. A non-finite sample is counted
// (nonfinite_samples()) and metered as silence: a NaN must not hold the
// K-weighting filters at NaN for the rest of the session.
//
// Readers: every reading is a relaxed atomic, so values from one moment may
// sit beside values one block older. A meter does not need more.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "sonora/dsp/loudness.h"
#include "sonora/dsp/true_peak.h"
#include "sonora/rt.h"

namespace sonora::dsp {

static_assert(std::atomic<float>::is_always_lock_free, "meter readings must be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "meter counters must be lock-free");

class TrackMeter {
public:
    // IEC 60268-10 Type I: 20 dB in 1.7 s.
    static constexpr double kIecReleaseDbPerS = 20.0 / 1.7;
    static constexpr float kFloorDb = -200.0f;

    struct Options {
        // BS.1770 channel weights, one per channel, or empty for 1.0 each.
        std::vector<double> weights;
        double release_db_per_s = kIecReleaseDbPerS;
        // Off: sample peak only. A track meter that shows no loudness skips
        // the K-weighting, the windows and the true-peak interpolator.
        bool loudness = true;
        bool true_peak = true;
    };

    TrackMeter() = default;
    TrackMeter(const TrackMeter&) = delete;
    TrackMeter& operator=(const TrackMeter&) = delete;

    // Allocates. max_frames bounds one process() call's scratch; a longer
    // call is metered in pieces. Returns false on a bad argument.
    bool prepare(double sample_rate, std::uint32_t max_frames, std::uint32_t channels,
                 const Options& options);
    bool prepare(double sample_rate, std::uint32_t max_frames, std::uint32_t channels) {
        return prepare(sample_rate, max_frames, channels, Options{});
    }

    // Audio thread. in[c] for c < channels().
    void process(const float* const* in, std::uint32_t frames) noexcept SONORA_RT;

    // Any thread: the next process() starts the integrated loudness, the
    // LRA, the maxima and the counters again (the operator's meter reset).
    void request_reset() noexcept { reset_requested_.store(true, std::memory_order_release); }

    std::uint32_t channels() const noexcept { return channels_; }

    // Readers, any thread, lock-free. Peaks are linear gain; take_* clear.
    float take_peak(std::uint32_t ch) noexcept;
    float display_peak(std::uint32_t ch) const noexcept;
    float take_true_peak(std::uint32_t ch) noexcept;
    float true_peak_max_dbtp() const noexcept { return load(tp_max_db_); }
    float momentary_lufs() const noexcept { return load(momentary_); }
    float short_term_lufs() const noexcept { return load(short_term_); }
    float max_momentary_lufs() const noexcept { return load(max_momentary_); }
    float max_short_term_lufs() const noexcept { return load(max_short_term_); }
    float integrated_lufs() const noexcept { return load(integrated_); }
    float loudness_range_lu() const noexcept { return load(lra_); }
    std::uint64_t nonfinite_samples() const noexcept {
        return nonfinite_.load(std::memory_order_relaxed);
    }
    // Completed 10 ms loudness steps since reset; a reader sees new values.
    std::uint64_t loudness_steps() const noexcept {
        return steps_.load(std::memory_order_relaxed);
    }

private:
    struct Channel {
        std::atomic<float> peak{0.0f};       // since the last take
        std::atomic<float> display{0.0f};    // ballistic
        std::atomic<float> true_peak{0.0f};  // since the last take
        double env = 0.0;                    // the ballistic, audio thread only
        double tp_max = 0.0;                 // since reset, audio thread only
    };
    static float load(const std::atomic<float>& a) noexcept {
        return a.load(std::memory_order_relaxed);
    }
    static void raise(std::atomic<float>& a, float v) noexcept;
    void reset_now() noexcept;
    void publish_loudness(std::uint64_t steps_before) noexcept;

    std::uint32_t channels_ = 0;
    std::uint32_t max_frames_ = 0;
    bool loudness_on_ = true;
    bool tp_on_ = true;
    double fall_ = 1.0;  // per-sample factor of the ballistic's fall
    std::unique_ptr<Channel[]> ch_;
    std::vector<float> scratch_;           // channels_ * max_frames_, finite input
    std::vector<const float*> scratch_ptr_;
    LoudnessMeter loudness_;
    TruePeakDetector tp_;

    std::atomic<bool> reset_requested_{false};
    std::atomic<std::uint64_t> nonfinite_{0};
    std::atomic<std::uint64_t> steps_{0};
    std::atomic<float> tp_max_db_{kFloorDb};
    std::atomic<float> momentary_{kFloorDb}, short_term_{kFloorDb};
    std::atomic<float> max_momentary_{kFloorDb}, max_short_term_{kFloorDb};
    std::atomic<float> integrated_{kFloorDb}, lra_{0.0f};
};

}  // namespace sonora::dsp
