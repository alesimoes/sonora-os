// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's rate converter (ADR-0111 §6): a capture runs at the rate it was
// trained at, so a session at another rate goes session -> capture before
// the network and capture -> session after it.
//
// Design: a rational polyphase FIR (Crochiere and Rabiner, "Multirate
// Digital Signal Processing", 1983, ch. 2-3) whose prototype is a
// Kaiser-windowed sinc (beta 9, about -90 dB stopband; J. O. Smith, "Digital
// Audio Resampling Home Page", CCRMA), the cabinet's offline design run as
// a stream. Every coefficient table is computed in design(), off the audio
// thread; process() is fixed arithmetic over a ring sized there.
//
// Why the delays are multiples of P (plan_rates): with fc / fs = P / Q
// reduced, both stages run on the same high rate H = P fs = Q fc. Each
// stage is linear-phase with its group delay d (in H samples) a multiple of
// P, so the pair delays the signal by exactly (d_up + d_down) / P session
// samples: an integer, which is what PDC needs (no fractional residue for
// the latency test's cross-correlation to round).
//
// Why the cutoff is 0.9 of the lower Nyquist with 40 zero crossings: the
// Kaiser transition (beta 9) is about 0.0897 x 32 / Z of the lower rate
// wide, so at Z = 40 the stopband starts at 0.482 of the lower rate, under
// its Nyquist (no image or alias above -90 dB), and the passband is flat to
// 0.418 of it: 18.4 kHz at 44.1 kHz.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/rt.h"

namespace sonora::native::amp {

constexpr double kResampleZeros = 40.0;
constexpr double kResampleCut = 0.9;    // of the lower rate's Nyquist
constexpr std::uint32_t kMaxRatioTerm = 4096;

// One stage: up by `up`, filter at the high rate, down by `down`.
class Resampler {
public:
    // Not real-time. `cut` is the cutoff relative to this stage's input
    // Nyquist (1: the input's band), `delay` the group delay in high-rate
    // samples, `max_in` the most input samples one process() call gets.
    void design(std::uint32_t up, std::uint32_t down, double cut, std::uint32_t delay, std::uint32_t max_in);

    // The most outputs one process() call of max_in inputs can make.
    std::uint32_t max_out() const noexcept { return max_out_; }
    std::uint32_t taps() const noexcept { return taps_; }

    void reset() noexcept SONORA_RT;
    // Pushes n inputs and writes every output they complete, at most `cap`
    // (a caller sized from max_out() never meets the cap). Returns the count.
    std::uint32_t process(const float* in, std::uint32_t n, float* out, std::uint32_t cap) noexcept SONORA_RT;

private:
    std::uint32_t up_ = 1, down_ = 1, taps_ = 1, max_out_ = 0;
    std::vector<float> coef_;  // [up][taps], oldest input first
    std::vector<float> hist_;  // [2 taps]: the ring, doubled so a window is contiguous
    std::uint32_t write_ = 0;
    // The next output's position after the newest input, in high-rate
    // samples; an output is due while it is under `up`.
    std::uint32_t t_ = 0;
};

// How a capture rate is reached from a session rate.
struct RatePlan {
    bool resample = false;
    std::uint32_t p = 1, q = 1;     // capture / session = p / q, reduced
    std::uint32_t delay_up = 0;     // session -> capture stage, high-rate samples
    std::uint32_t delay_down = 0;   // capture -> session stage
    double cut_up = 1.0, cut_down = 1.0;
    std::uint32_t latency = 0;      // session samples, exact
};

// False with *error set (one sentence) when the rates are not whole
// numbers of hertz or their ratio needs a term over kMaxRatioTerm.
bool plan_rates(double session_rate, double capture_rate, RatePlan* plan, std::string* error);

}  // namespace sonora::native::amp
