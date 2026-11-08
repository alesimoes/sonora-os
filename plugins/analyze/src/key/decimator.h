// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming sample-rate converter to the key pass's analysis rate: a
// Hann-windowed sinc (8 zero crossings, cutoff 0.45 of the output rate),
// evaluated from a table of 512 fractional phases with linear interpolation
// so that any input rate (44.1, 48, 96 kHz...) converts without a per-tap
// sin(). The chroma needs only 65 Hz to 2 kHz, so the stopband need not be
// steep: anything that aliases into that range comes from above ~9 kHz, which
// the window attenuates by more than 40 dB. Offline; it allocates.
#pragma once

#include <cstdint>
#include <vector>

namespace sonora::analyze::key {

class Decimator {
public:
    static constexpr int kPhases = 512;
    static constexpr int kZeroCrossings = 8;

    bool prepare(double in_rate, double out_rate);
    // Appends the output samples that `in` completes to `out`.
    void process(const float* in, std::size_t n, std::vector<float>& out);

private:
    double step_ = 1.0;  // input samples per output sample
    int half_ = 1;       // taps on each side of the output instant
    double next_ = 0.0;  // input-time of the next output
    std::int64_t base_ = 0;  // input index of buf_[0]
    std::vector<float> buf_;
    std::vector<float> table_;  // (kPhases + 1) x (2 * half_)
};

}  // namespace sonora::analyze::key
