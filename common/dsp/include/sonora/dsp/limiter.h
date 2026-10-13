// SPDX-License-Identifier: GPL-3.0-or-later
//
// Look-ahead peak limiter with true-peak detection (BS.1770-5 Annex 2).
//
// Technique: U. Zolzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley,
// 2011, ch. 4 (look-ahead limiting), and J. Reiss and A. McPherson, *Audio
// Effects*, CRC, 2014, ch. 6. The audio is delayed by the look-ahead; the
// gain each sample needs is computed from the 4x-interpolated signal; a
// sliding minimum holds it across the look-ahead window and two cascaded
// moving averages of the held value turn the step into a smooth ramp that
// is complete before the peak arrives. Because every value the averages
// see is at or below the gain the peak needs, the applied gain is never
// above it, which is a property of the arithmetic rather than of tuning.
//
// What the ceiling test (ADR-0011 §3.2, a 16x reference meter with a longer
// support than ours) taught, each change measured in tests/test_limiter.cpp:
// - A 4x grid misses peaks between its points by up to 0.5 dB at 20 kHz;
//   the detector refines each local maximum by a sinusoidal fit.
// - A 32-tap-per-phase interpolator under-reads 19-22 kHz content; the
//   limiter's has 96 taps per phase.
// - What remains on band-limited material is at most 0.023 dB; the target
//   sits kSafetyDb = 0.05 dB under the ceiling.
// - Content at full scale up to Nyquist (unfiltered white noise) still
//   reads up to +0.7 dB on the reference. It is reported, not asserted:
//   programme material is band-limited, and no finite detector sees it all.
// The hold window is also widened by kGuard samples each side, so the gain
// is at or below a peak's need around it, not only at it.
//
// Latency is look-ahead + the interpolator's delay, fixed at prepare(); the
// true-peak switch does not change it (sample-peak mode reads phase 0 of the
// same interpolator), so toggling it never moves the audio.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/dsp/true_peak.h"
#include "sonora/rt.h"

namespace sonora::dsp {

class LookaheadLimiter {
public:
    static constexpr std::uint32_t kGuard = 4;
    static constexpr std::uint32_t kMaxChannels = 16;
    static constexpr std::uint32_t kDetectorTaps = 96;
    // The limiter aims this far under the set ceiling (see process_frame).
    static constexpr double kSafetyDb = 0.05;
    static constexpr double kSafetyGain = 0.9942600739529567;  // 10^(-0.05/20)

    // Allocates. lookahead_samples >= kGuard + 1.
    bool prepare(double sample_rate, std::uint32_t channels, std::uint32_t lookahead_samples,
                 std::uint32_t detector_taps_per_phase = kDetectorTaps,
                 std::uint32_t guard = kGuard);
    void reset() noexcept;
    std::uint32_t latency() const noexcept { return lookahead_ + tp_.delay(); }
    std::uint32_t lookahead() const noexcept { return lookahead_; }

    void set_ceiling(double linear) noexcept { ceiling_ = linear; }
    void set_release(double seconds) noexcept;
    void set_true_peak(bool on) noexcept { true_peak_ = on; }
    void set_linked(bool on) noexcept { linked_ = on; }

    // In place. Ceiling and release may change between calls.
    void process(float* const* io, std::uint32_t frames) noexcept SONORA_RT;
    // Per-sample variant for callers that smooth the ceiling per sample.
    void process_frame(float* frame) noexcept SONORA_RT;

    // Deepest gain reduction since the last call, in dB (<= 0).
    double take_gain_reduction_db() noexcept;

private:
    struct Path {
        // Sliding minimum (monotonic deque) over the hold window.
        std::vector<double> dq_val;
        std::vector<std::uint64_t> dq_idx;
        std::uint32_t dq_head = 0, dq_size = 0;
        // Moving average of the held gain.
        std::vector<double> avg_ring;
        std::uint32_t avg_pos = 0;
        double avg_sum = 0.0;
        std::vector<double> avg2_ring;
        std::uint32_t avg2_pos = 0;
        double avg2_sum = 0.0;
        double released = 1.0;
        double gain = 1.0;
    };
    double hold_and_smooth(Path& p, double need) noexcept;

    double rate_ = 48000.0;
    std::uint32_t channels_ = 0;
    std::uint32_t lookahead_ = 0;
    std::uint32_t guard_ = kGuard;
    std::uint32_t hold_len_ = 0;  // W = lookahead + kGuard + 2
    std::uint32_t avg_len_ = 0;   // the two averages span M = L - G + 1
    std::uint32_t avg2_len_ = 0;
    double ceiling_ = 1.0;
    double release_coeff_ = 0.0;
    bool true_peak_ = true;
    bool linked_ = true;
    std::uint64_t n_ = 0;

    TruePeakDetector tp_;
    std::vector<Path> paths_;            // one when linked use path 0; else per channel
    std::vector<std::vector<float>> delay_;
    std::uint32_t delay_pos_ = 0;
    std::vector<double> frame_need_;
    double min_gain_seen_ = 1.0;
};

}  // namespace sonora::dsp
