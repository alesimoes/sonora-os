// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loudness meter: ITU-R BS.1770-5 with the EBU R 128 "EBU Mode" readings
// (EBU Tech 3341, v4 2023) and Loudness Range (EBU Tech 3342, v4 2023).
//
// - K-weighting per BS.1770 Annex 1: a high-shelf pre-filter and the RLB
//   high-pass. The standard prints coefficients for 48 kHz only. Other rates
//   use the analog parameters that reproduce those coefficients through the
//   bilinear transform (f0, Q and gain of each stage, as fitted and
//   published in libebur128, MIT licence, ebur128.c). A test checks that at
//   48 kHz they give the printed coefficients.
// - Channel weights: 1.0 for L, R, C; 1.41 for the surrounds; 0 for LFE.
// - Momentary: 400 ms window. Short-term: 3 s window. Both are evaluated
//   every 10 ms, finer than Tech 3341's 10 Hz minimum, so a 400 ms tone
//   read by a live meter is never off its peak by more than 5 ms (test
//   cases 13 and 14).
// - Integrated: 400 ms blocks every 100 ms (75 % overlap), absolute gate
//   -70 LUFS, relative gate -10 LU.
// - Loudness Range: short-term values every 100 ms, absolute gate -70 LUFS,
//   relative gate -20 LU, 95th minus 10th percentile, with the percentile
//   rule of Tech 3342's reference code.
// - True peak per BS.1770 Annex 2 (true_peak.h).
//
// The integrated and LRA statistics need every block since reset, which is
// unbounded. They are kept as histograms of 0.01 LU bins from -70 to +30
// LUFS, holding a count and the exact energy sum per bin, so the integrated
// value averages exact energies and only the gate decision is quantised.
// That keeps process() allocation-free for any programme length.
//
// The histograms are Fenwick trees (binary indexed trees; P. M. Fenwick,
// "A new data structure for cumulative frequency tables", Software:
// Practice and Experience 24(3), 1994), so a sum over the bins above a gate
// and the bin holding a given rank cost O(log bins), 14 steps, instead of a
// 10 000-bin scan. That is what lets a track meter (meter.h) publish the
// integrated loudness and the LRA from the audio thread every 100 ms. Counts
// live in a forward tree, so a sum above the gate is an exact integer
// difference; energies live in a tree over the reversed bins, so the sum
// above the gate is a prefix sum and never a difference of large numbers.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/true_peak.h"
#include "sonora/rt.h"

namespace sonora::dsp {

// The two K-weighting stages for a sample rate (BS.1770 Annex 1). Stage 2's
// numerator is (1, -2, 1) unnormalised, exactly as the standard prints it.
void k_weighting(double sample_rate, BiquadCoeffs& stage1, BiquadCoeffs& stage2) noexcept;

class LoudnessMeter {
public:
    static constexpr double kFloorLufs = -200.0;

    // weights: one per channel, or null for 1.0 each. true_peak: false when
    // the caller measures it itself (meter.h does, per read), which saves
    // the interpolator, most of the per-sample cost. Allocates.
    bool prepare(double sample_rate, std::uint32_t channels, const double* weights = nullptr,
                 bool true_peak = true);
    void reset() noexcept SONORA_RT;
    void process(const float* const* in, std::uint32_t frames) noexcept SONORA_RT;

    double momentary_lufs() const noexcept { return momentary_; }
    double short_term_lufs() const noexcept { return short_term_; }
    double max_momentary_lufs() const noexcept { return max_momentary_; }
    double max_short_term_lufs() const noexcept { return max_short_term_; }
    // O(log bins): cheap enough for the audio thread every 100 ms. Not
    // safe to call from another thread while process() runs.
    double integrated_lufs() const noexcept SONORA_RT;
    double loudness_range_lu() const noexcept SONORA_RT;
    double true_peak_dbtp() const noexcept;
    // Number of 10 ms steps completed since reset; readers use it to see a
    // new momentary/short-term value.
    std::uint64_t steps() const noexcept { return steps_; }

private:
    static constexpr double kHistMin = -70.0;
    static constexpr double kHistMax = 30.0;
    static constexpr double kBin = 0.01;
    static constexpr std::uint32_t kSubsPerMomentary = 40;   // 400 ms / 10 ms
    static constexpr std::uint32_t kSubsPerShortTerm = 300;  // 3 s / 10 ms
    static constexpr std::uint32_t kSubsPerGateStep = 10;    // 100 ms

    // Block counts and energies per 0.01 LU bin, as Fenwick trees.
    struct Hist {
        std::vector<std::uint64_t> count;  // 1-based tree over bins 0..n-1
        std::vector<double> energy;        // 1-based tree over bins n-1..0
        std::uint32_t n = 0;
        std::uint32_t top = 0;  // the largest power of two <= n
        void resize(std::uint32_t bins);
        void clear() noexcept SONORA_RT;
        void add(double lufs, double energy) noexcept SONORA_RT;
        std::uint32_t bins() const noexcept { return n; }
        // Blocks in bins [0, i).
        std::uint64_t count_below(std::uint32_t i) const noexcept SONORA_RT;
        // Energy summed over bins [i, n).
        double energy_from(std::uint32_t i) const noexcept SONORA_RT;
        // The bin holding the rank-th block counted from the bottom, rank >= 1.
        std::uint32_t bin_of_rank(std::uint64_t rank) const noexcept SONORA_RT;
        // The first bin whose lower edge is at or above `lufs`, or n.
        std::uint32_t first_bin_at_or_above(double lufs) const noexcept SONORA_RT;
    };
    void finish_sub_block() noexcept SONORA_RT;
    static double to_lufs(double mean_square) noexcept SONORA_RT;
    bool measure_tp_ = true;

    double rate_ = 48000.0;
    std::uint32_t channels_ = 0;
    std::vector<double> weight_;
    BiquadCoeffs k1_, k2_;
    std::vector<BiquadState> s1_, s2_;
    TruePeakDetector tp_;
    std::vector<double> tp_max_;

    std::uint32_t sub_len_ = 480;
    std::uint32_t sub_fill_ = 0;
    double sub_sum_ = 0.0;
    std::vector<double> ring_;  // mean square per 10 ms sub-block
    std::uint32_t ring_pos_ = 0;
    std::uint64_t steps_ = 0;

    double momentary_ = kFloorLufs, short_term_ = kFloorLufs;
    double max_momentary_ = kFloorLufs, max_short_term_ = kFloorLufs;
    Hist gate_hist_, lra_hist_;
};

}  // namespace sonora::dsp
