// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming rational rate conversion (ADR-0180), shared by every processor
// that runs a fixed-rate stage inside a session at another rate: the amp's
// captures (ADR-0111 §6) first, the cabinet and the budget IRs next. Moved
// here from plugins/native/amp so there is one design and one test.
//
// Design: a rational polyphase FIR (Crochiere and Rabiner, "Multirate
// Digital Signal Processing", 1983, ch. 2-3) whose prototype is a
// Kaiser-windowed sinc (beta 9, about -90 dB stopband; J. O. Smith, "Digital
// Audio Resampling Home Page", CCRMA). Every coefficient table is computed
// in prepare(), off the audio thread; process() is fixed arithmetic over a
// ring sized there: no allocation, no lock, no throw.
//
// Why the delays are multiples of P (plan_rates): with fc / fs = P / Q
// reduced, both stages run on the same high rate H = P fs = Q fc. Each
// stage is linear-phase with its group delay d (in H samples) a multiple of
// P, so the pair delays the signal by exactly (d_up + d_down) / P session
// samples: an integer, which is what PDC needs (no fractional residue for
// the latency test's cross-correlation to round). RatePlan::latency is
// that figure, and tests/test_resample.cpp measures it.
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

namespace sonora::dsp {

constexpr double kResampleZeros = 40.0;
constexpr double kResampleCut = 0.9;  // of the lower rate's Nyquist
constexpr std::uint32_t kMaxRatioTerm = 4096;

// One stage: up by `up`, filter at the high rate, down by `down`.
class Resampler {
public:
    // The coefficient table's bound (up x taps floats): 16 MB, far over
    // any plan_rates() stage (the worst, 192 kHz <-> 44.1 kHz, is 57 k).
    static constexpr std::uint64_t kMaxCoefficients = std::uint64_t{1} << 22;

    // Not real-time; allocates. `cut` is the cutoff relative to this
    // stage's input Nyquist (1: the input's band), `delay` the group delay
    // in high-rate samples, `max_in` the most input samples one process()
    // call gets. False (the stage unchanged) when a factor is 0 or over
    // kMaxRatioTerm, cut is not in (0, 1], max_in is 0, or the table would
    // pass kMaxCoefficients.
    bool prepare(std::uint32_t up, std::uint32_t down, double cut, std::uint32_t delay, std::uint32_t max_in);

    // The most outputs one process() call of max_in inputs can make.
    std::uint32_t max_out() const noexcept { return max_out_; }
    std::uint32_t taps() const noexcept { return taps_; }
    std::uint32_t up() const noexcept { return up_; }
    std::uint32_t down() const noexcept { return down_; }
    // The group delay, high-rate samples.
    std::uint32_t delay() const noexcept { return delay_; }

    void reset() noexcept SONORA_RT;
    // Pushes n inputs and writes every output they complete, at most `cap`
    // (a caller sized from max_out() never meets the cap). Returns the
    // count. A non-finite input is read as 0, so the output stays finite.
    std::uint32_t process(const float* in, std::uint32_t n, float* out, std::uint32_t cap) noexcept SONORA_RT;

private:
    std::uint32_t up_ = 1, down_ = 1, taps_ = 1, delay_ = 0, max_out_ = 0;
    std::vector<float> coef_;  // [up][taps], oldest input first
    std::vector<float> hist_;  // [2 taps]: the ring, doubled so a window is contiguous
    std::uint32_t write_ = 0;
    // The next output's position after the newest input, in high-rate
    // samples; an output is due while it is under `up`.
    std::uint32_t t_ = 0;
};

// How a fixed processing rate (`capture`) is reached from a session rate.
struct RatePlan {
    bool resample = false;
    std::uint32_t p = 1, q = 1;    // capture / session = p / q, reduced
    std::uint32_t delay_up = 0;    // session -> capture stage, high-rate samples
    std::uint32_t delay_down = 0;  // capture -> session stage
    double cut_up = 1.0, cut_down = 1.0;
    std::uint32_t latency = 0;     // session samples, exact
};

// Not real-time. False with *error set (one sentence) when the rates are
// not whole numbers of hertz in [1, 768] kHz or their ratio needs a term
// over kMaxRatioTerm. Equal rates plan no resampling and no latency.
bool plan_rates(double session_rate, double capture_rate, RatePlan* plan, std::string* error);

// Prepares the pair a plan describes: `up` takes session blocks of at most
// max_in, `down` takes whatever `up` makes. False when either refuses.
bool prepare_pair(const RatePlan& plan, std::uint32_t max_in, Resampler* up, Resampler* down);

}  // namespace sonora::dsp
