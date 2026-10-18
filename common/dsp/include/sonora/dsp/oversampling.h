// SPDX-License-Identifier: GPL-3.0-or-later
//
// 2x, 4x and 8x oversampling by cascaded polyphase half-band FIR stages,
// with an exact integer latency.
//
// Why FIR and not the cheaper polyphase IIR half-band (allpass pairs): a
// linear-phase stage has one group delay at every frequency, so the
// oversampler's latency is a number of samples, and the PDC contract
// (ADR-0011 §1.5) can be met to the sample. An IIR half-band's delay
// varies with frequency, and "the latency" would be a choice of frequency.
//
// Each stage is a Kaiser-windowed half-band (J. F. Kaiser, 1974; the
// half-band structure as in P. P. Vaidyanathan, *Multirate Systems and
// Filter Banks*, 1993, §4.6): every other tap is zero and the centre tap is
// 1/2, so per output sample a stage costs half its length, and one of the
// two polyphase branches is a pure delay. Stage s runs between 2^s and
// 2^(s+1) times the base rate; its transition band is everything between
// the passband edge and the first image, so later stages are much shorter
// than the first.
//
// Latency. A stage with 4K-1 taps delays by 2K-1 samples at its high rate
// on the way up and again on the way down. Summed over the stages this is
// not always a whole number of base-rate samples, so a short delay at the
// top rate pads it to one. latency() is that whole number, and a test
// measures it at every factor and rate.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp {

class Oversampler {
public:
    // factor in {1, 2, 4, 8}. passband_ratio is the top of the protected
    // band as a fraction of the base rate (20 kHz at 44.1 kHz = 0.4535).
    // stopband_db is the first stage's image rejection. Allocates.
    bool prepare(std::uint32_t factor, std::uint32_t channels,
                 std::uint32_t max_frames, double passband_ratio = 0.4535,
                 double stopband_db = 100.0);
    // Zeroes the histories prepare() sized: no allocation, callable from a
    // processor's on_reset() on the audio thread.
    void reset() noexcept SONORA_RT;

    std::uint32_t factor() const noexcept { return factor_; }
    // Round-trip delay (up then down), in base-rate samples.
    std::uint32_t latency() const noexcept { return latency_; }
    // Delay of up() alone, in samples at the top rate (factor x base):
    // base-rate sample n appears at top-rate index factor * n + up_delay().
    // Not always a multiple of the factor, so a detector that reads the
    // oversampled signal without coming back down (TruePeakGuard in
    // limiter.h) aligns itself with this rather than with latency().
    std::uint32_t up_delay() const noexcept { return up_delay_; }
    // Taps per stage, first stage first. For reports.
    const std::vector<std::uint32_t>& stage_taps() const noexcept { return taps_; }

    // Upsamples `frames` base-rate samples of channel `ch`. Returns the
    // oversampled buffer (frames * factor samples), which the caller may
    // process in place before down().
    double* up(std::uint32_t ch, const float* in, std::uint32_t frames) noexcept SONORA_RT;
    // Decimates channel ch's oversampled buffer back into `out`.
    void down(std::uint32_t ch, float* out, std::uint32_t frames) noexcept SONORA_RT;

private:
    struct Stage {
        std::uint32_t k = 0;            // taps = 4k - 1
        std::vector<double> even;       // 2k non-centre taps h[2i]
    };
    struct ChannelStage {
        std::vector<double> up_hist;    // last 2k inputs, doubled for wrap-free reads
        std::uint32_t up_pos = 0;
        std::vector<double> dn_even;    // even high-rate samples
        std::vector<double> dn_odd;     // odd high-rate samples
        std::uint32_t dn_pos = 0;
        std::uint32_t dn_opos = 0;
    };
    struct Channel {
        std::vector<ChannelStage> stages;
        std::vector<std::vector<double>> bufs;  // bufs[s] at 2^s times the base rate
        std::vector<double> pad;                // top-rate delay
        std::uint32_t pad_pos = 0;
    };

    std::uint32_t factor_ = 1;
    std::uint32_t latency_ = 0;
    std::uint32_t up_delay_ = 0;
    std::uint32_t pad_ = 0;
    std::vector<Stage> stages_;
    std::vector<std::uint32_t> taps_;
    std::vector<Channel> ch_;
};

}  // namespace sonora::dsp
