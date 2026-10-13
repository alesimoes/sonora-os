// SPDX-License-Identifier: GPL-3.0-or-later
//
// Look-ahead peak limiter with true-peak detection (BS.1770-5 Annex 2), and
// the oversampled true-peak guard that follows it.
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
// - Content at full scale up to Nyquist (unfiltered white noise) still read
//   +0.68 dB over on the reference: the detector's interpolator rolls off
//   from 0.47 fs, and the limiter cannot see the sidebands its own gain
//   puts on the audio. TruePeakGuard, below, is the stage that removes it.
// The hold window is also widened by kGuard samples each side, so the gain
// is at or below a peak's need around it, not only at it.
//
// Latency is look-ahead + the interpolator's delay, fixed at prepare(); the
// true-peak switch does not change it (sample-peak mode reads phase 0 of the
// same interpolator), so toggling it never moves the audio.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/true_peak.h"
#include "sonora/rt.h"

namespace sonora::dsp {

// The gain path a look-ahead stage runs on its per-sample need: a sliding
// minimum over W = L + G + 2 needs, a release, and two cascaded moving
// averages spanning M = L - G + 1 in all. One per channel, or one for a
// linked group. The window arithmetic is in limiter.cpp.
class LookaheadGain {
public:
    // Allocates. lookahead >= guard + 1.
    void prepare(std::uint32_t lookahead, std::uint32_t guard);
    void reset() noexcept;
    void set_release_coeff(double c) noexcept { release_coeff_ = c; }
    // The need computed at this step in, the gain for the sample leaving
    // now out.
    double next(double need) noexcept SONORA_RT;

private:
    std::uint32_t hold_len_ = 0;
    std::uint32_t avg_len_ = 1;
    std::uint32_t avg2_len_ = 1;
    double release_coeff_ = 0.0;
    std::uint64_t n_ = 0;
    // Sliding minimum (monotonic deque) over the hold window.
    std::vector<double> dq_val_;
    std::vector<std::uint64_t> dq_idx_;
    std::uint32_t dq_head_ = 0, dq_size_ = 0;
    std::vector<double> avg_ring_;
    std::uint32_t avg_pos_ = 0;
    double avg_sum_ = 0.0;
    std::vector<double> avg2_ring_;
    std::uint32_t avg2_pos_ = 0;
    double avg2_sum_ = 0.0;
    double released_ = 1.0;
};

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
    double rate_ = 48000.0;
    std::uint32_t channels_ = 0;
    std::uint32_t lookahead_ = 0;
    double ceiling_ = 1.0;
    bool true_peak_ = true;
    bool linked_ = true;

    TruePeakDetector tp_;
    std::vector<LookaheadGain> paths_;  // one per channel; linked, all get the same need
    std::vector<std::vector<float>> delay_;
    std::uint32_t delay_pos_ = 0;
    std::vector<double> frame_need_;
    double min_gain_seen_ = 1.0;
};

// The ceiling guarantee: the last stage of a mastering limiter, after
// LookaheadLimiter, so the ceiling holds whatever the first stage missed
// (coordinator's decision on PR #34: a mastering limiter never exceeds its
// ceiling). Measured in tests/test_limiter.cpp and recorded in
// brain/research/native-dsp-n0-measurements.md.
//
// What it sees that the first stage cannot:
// - the first stage's output, so the sidebands that stage's gain puts on
//   the audio are measured, not predicted;
// - content up to Nyquist. Detection runs at 8x through the half-band
//   cascade, whose first stage is built to be the reference meter's own
//   filter (reference_peak.h: a Kaiser-windowed sinc, 120 dB, +-256
//   samples; here 4 x 256 - 1 taps at 2x, the same window over the same
//   span). With a 0.49 fs passband at 80 dB instead, full-band white noise
//   still read up to +0.13 dB over on the reference: content in the last
//   1 % below Nyquist was read through two different transition bands.
//   Each local maximum of the 8x points is refined by the sinusoidal fit
//   TruePeakDetector uses, capped at 1 / cos(pi / 16), the most a
//   band-limited peak rises over an 8x point;
// - what a BS.1770 meter reads. Its 4x interpolator passes part of the
//   images of near-Nyquist content and reads full-band noise up to 2.9 dB
//   above the reference (+0.29 dB over the ceiling after the first
//   stage), so the meter's own reading is a second need.
//
// The gain computer is LookaheadGain at the first stage's 0.05 dB target,
// so where the first stage already holds the ceiling it does nothing at
// all. Where it acts, it acts by gain ramped over its own look-ahead, never
// by clipping, so it adds no harmonics of its own. Its gain also puts
// sidebands on near-Nyquist content, which it cannot see because it
// detects before applying it; their effect grows with the depth of the
// gain change, so it aims deeper in proportion, at the need in dB times
// kDepthExponent (1.0 left -0.028 dB of the 0.05 dB margin on full-band
// noise; 1.15 keeps it all).
//
// Last of all, every output sample is clamped to the ceiling that was in
// force when it entered. That only acts on a sample peak above the
// ceiling, which the gain stage has already prevented; clamped() counts
// it, and the tests assert it stays zero.
//
// Latency: look-ahead + the 8x detector's alignment delay, fixed at
// prepare(); neither the true-peak switch nor the link changes it.
class TruePeakGuard {
public:
    static constexpr std::uint32_t kFactor = 8;
    // The first stage then has k = 256 (4k - 1 = 1023 taps at 2x).
    static constexpr double kPassband = 0.49236;
    static constexpr double kStopbandDb = 120.0;
    static constexpr double kDepthExponent = 1.15;
    // One more than the 6 it gives: the 8x points of one step straddle two
    // base-rate intervals, so each sample is touched by three steps' needs
    // rather than the first stage's two (window arithmetic in limiter.cpp).
    static constexpr std::uint32_t kGuard = 7;

    // Allocates. lookahead_samples >= kGuard + 1.
    bool prepare(double sample_rate, std::uint32_t channels, std::uint32_t max_frames,
                 std::uint32_t lookahead_samples);
    void reset() noexcept;
    std::uint32_t latency() const noexcept { return align_ + lookahead_; }

    void set_release(double seconds) noexcept;
    void set_true_peak(bool on) noexcept { true_peak_ = on; }
    void set_linked(bool on) noexcept { linked_ = on; }

    // In place, frames <= max_frames. ceiling[i] is the linear ceiling for
    // input sample i, as the first stage was given it.
    void process(float* const* io, const double* ceiling, std::uint32_t frames) noexcept SONORA_RT;

    double take_gain_reduction_db() noexcept;
    // Output samples the final clamp changed since prepare() or reset().
    std::uint64_t clamped() const noexcept { return clamped_; }

private:
    double rate_ = 48000.0;
    std::uint32_t channels_ = 0;
    std::uint32_t lookahead_ = 0;
    std::uint32_t align_ = 0;  // base-rate delay that lines the audio up with its points
    bool true_peak_ = true;
    bool linked_ = true;

    Oversampler os_;
    TruePeakDetector meter_;  // what a BS.1770 meter reads, on the audio delayed to line up
    std::vector<LookaheadGain> paths_;
    std::vector<double> prev_;                // per channel, the last two 8x points
    std::vector<std::vector<float>> audio_;   // align_ + lookahead_ + 1 per channel
    std::vector<double> ceil_line_;           // the ceiling, delayed with the audio
    std::uint32_t pos_ = 0;
    std::vector<double> need_;
    double min_gain_seen_ = 1.0;
    std::uint64_t clamped_ = 0;
};

}  // namespace sonora::dsp
