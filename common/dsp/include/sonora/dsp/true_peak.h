// SPDX-License-Identifier: GPL-3.0-or-later
//
// True-peak detection per ITU-R BS.1770-5, Annex 2: oversample by 4, low-pass,
// take the absolute value. The standard notes that the 12.04 dB attenuation
// before oversampling exists for fixed-point headroom; in floating point it
// is unnecessary and omitted.
//
// The interpolator is our own design, not the Annex's 48-tap example: a
// Kaiser-windowed sinc with T taps per phase (4 T + 1 in all). The meter
// uses T = 32, which passes every EBU Tech 3341 true-peak case. The
// limiter uses a longer one: its transition band must sit closer to
// Nyquist, or content between 20 kHz and Nyquist is under-read and passes
// as an over (measured in tests/test_limiter.cpp). Phase 0 of a windowed
// sinc is a pure delay, so the detector also hands back the original
// sample, and the limiter's sample-peak mode costs nothing extra.
//
// Timing: after the input sample x[n], the four interpolated points cover
// base-rate times [n - D, n - D + 1) in quarter steps, D = delay() = T / 2.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp {

class TruePeakDetector {
public:
    static constexpr std::uint32_t kFactor = 4;

    // taps_per_phase must be even. Allocates.
    //
    // refine: a 4x grid under-reads a peak that falls between its points,
    // by up to 1 - cos(pi f / 4 fs): 0.5 dB at 20 kHz and 48 kHz. That is
    // inside BS.1770's tolerance for a meter, and is what a meter reports.
    // A limiter must not pass it, so with refine every local maximum of the
    // points is replaced by the amplitude of the sinusoid through it and its
    // two neighbours, which is exact for a sinusoid. Measured in
    // tests/test_limiter.cpp against a 16x reference.
    void prepare(std::uint32_t channels, std::uint32_t taps_per_phase = 32,
                 double stopband_db = 80.0, bool refine = false);
    void reset() noexcept;
    std::uint32_t delay() const noexcept { return taps_ / 2; }

    // Feeds one sample. Returns the largest |value| among the four
    // interpolated points (refined if prepared so); `sample_peak` receives
    // |x[n - D]|. With refine, a peak whose fit straddles two samples
    // may be reported one sample late; the limiter's guard covers that.
    double process(std::uint32_t ch, double x, double* sample_peak = nullptr) noexcept SONORA_RT;

    // The interpolated points themselves: out[p] is the value at time
    // n - D + p / 4.
    void process_points(std::uint32_t ch, double x, double* out4) noexcept SONORA_RT;

    // The peak of the four points just produced for channel ch: their
    // largest |value|, refined by the sinusoidal fit if prepared so. Call
    // once per process_points(), in order; it keeps the last two points.
    double peak_of(std::uint32_t ch, const double* pts4) noexcept SONORA_RT;

private:
    std::uint32_t taps_ = 32;
    // phase_[p][i] = h[p + 4 i] for p = 1..3; phase 0 is the delay.
    std::vector<double> phase_[kFactor];
    bool refine_ = false;
    struct Hist {
        std::vector<double> x;  // doubled ring of taps_
        std::uint32_t pos = 0;
        double prev2 = 0.0, prev3 = 0.0;  // points 2 and 3 of the last sample
    };

    std::vector<Hist> hist_;
};

}  // namespace sonora::dsp
