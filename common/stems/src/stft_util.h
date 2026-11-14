#pragma once
// Private STFT helpers for the offline separators (ADR-0165). Hann analysis and
// synthesis at 75% overlap: sum of w^2 is the constant 1.5 away from the edges,
// and the signal is zero-padded by one frame each side so every sample sees it.
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "sonora/stems/stem_separator.h"

namespace sonora::stems::detail {

constexpr int kN = 2048;
constexpr int kHop = kN / 4;
constexpr int kBins = kN / 2 + 1;
constexpr double kPi = 3.14159265358979323846;

inline std::int64_t stft_frames(std::int64_t frames) {
    return (frames + 2 * kN) / kHop + 1;
}

class Stft {
public:
    Stft() : win_(kN), tw_(kN / 2), buf_(kN), rev_(kN) {
        for (int i = 0; i < kN; ++i) win_[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / kN);
        for (int i = 0; i < kN / 2; ++i) tw_[i] = std::polar(1.0, -2 * kPi * i / kN);
        int bits = 0;
        while ((1 << bits) < kN) ++bits;
        for (int i = 0; i < kN; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[i] = r;
        }
    }

    // Spectrum of frame f of x (signal index = f*hop + n - kN) into bins.
    void analyze(const float* x, std::int64_t frames, std::int64_t f,
                 std::complex<double>* bins) {
        for (int n = 0; n < kN; ++n) {
            std::int64_t i = f * kHop + n - kN;
            double v = (i >= 0 && i < frames) ? x[i] : 0.0;
            buf_[rev_[n]] = v * win_[n];
        }
        fft(false);
        for (int k = 0; k < kBins; ++k) bins[k] = buf_[k];
    }

    // Overlap-adds frame f's bins into out[0..frames) (double accumulator).
    void synthesize(const std::complex<double>* bins, std::int64_t frames,
                    std::int64_t f, double* out) {
        for (int k = 0; k < kBins; ++k) buf_[rev_[k]] = bins[k];
        for (int k = kBins; k < kN; ++k) buf_[rev_[k]] = std::conj(bins[kN - k]);
        fft(true);
        for (int n = 0; n < kN; ++n) {
            std::int64_t i = f * kHop + n - kN;
            if (i >= 0 && i < frames) out[i] += buf_[n].real() * win_[n] / (1.5 * kN);
        }
    }

private:
    // buf_ is already bit-reversed.
    void fft(bool inverse) {
        for (int len = 2; len <= kN; len <<= 1) {
            int step = kN / len;
            for (int s = 0; s < kN; s += len)
                for (int j = 0; j < len / 2; ++j) {
                    std::complex<double> w = tw_[j * step];
                    if (inverse) w = std::conj(w);
                    auto a = buf_[s + j];
                    auto b = buf_[s + j + len / 2] * w;
                    buf_[s + j] = a + b;
                    buf_[s + j + len / 2] = a - b;
                }
        }
    }
    std::vector<double> win_;
    std::vector<std::complex<double>> tw_, buf_;
    std::vector<int> rev_;
};

inline bool valid_request(const StemRequest& r) {
    return r.in && r.channels > 0 && r.frames >= 0 && r.sample_rate > 0;
}

}  // namespace sonora::stems::detail
