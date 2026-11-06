// SPDX-License-Identifier: GPL-3.0-or-later
//
// Band splitting for multiband processors, in two designs (ADR-0011 §2.1).
//
// LrBank, minimum phase: Linkwitz-Riley fourth-order crossovers (S. H.
// Linkwitz, "Active crossover networks for noncoincident drivers", JAES
// 24(1), 1976), each leg two cascaded Butterworth sections, as trapezoidal
// state-variable filters (the bilinear transform prewarped at the crossover,
// so every leg is -6.02 dB there; the cramping of a bilinear design is above
// the crossover and does not touch the sum). With LP and HP of a fourth-order
// Linkwitz-Riley, LP + HP = D(-s) / D(s): an exact second-order all-pass of
// the Butterworth denominator D. The bank is a cascade (each crossover splits
// the previous one's high leg) and the bands already split are run through
// that crossover's all-pass, so the sum of all bands is an all-pass of the
// input: flat in magnitude to rounding, with no latency in the sense of
// buffering (its phase delay is group delay, which a delay-compensating host
// does not add).
//
// linear_phase_lowpass, linear phase: a Kaiser-windowed sinc (J. F. Kaiser,
// 1974), unit gain at DC, with an odd number of taps so the delay is a whole
// (taps - 1) / 2 samples. A multiband built on N low-passes takes band k as
// LP(k) - LP(k - 1) and the top band as the input delayed by the same amount
// minus the last low-pass, so the bands sum to the delayed input exactly
// (a telescoping sum), whatever the windowing does to the slopes.
#pragma once

#include <cmath>
#include <cstdint>

#include "sonora/dsp/fft.h"
#include "sonora/rt.h"

namespace sonora::dsp {

constexpr std::uint32_t kMaxCrossoverBands = 4;
constexpr std::uint32_t kMaxCrossoverChannels = 2;

// Orders crossover frequencies so a band is never narrower than `ratio` and
// none passes `0.45 * sample_rate`; at most kMaxCrossoverBands - 1 values.
inline void order_crossovers(double* hz, std::uint32_t count, double sample_rate,
                             double min_ratio = 1.25) noexcept {
    const double top = 0.45 * sample_rate;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i > 0 && hz[i] < hz[i - 1] * min_ratio) hz[i] = hz[i - 1] * min_ratio;
        if (hz[i] > top) hz[i] = top;
    }
    // Squeezed against Nyquist: back down so they stay ascending.
    for (std::uint32_t i = count; i-- > 1;) {
        if (hz[i - 1] > hz[i] / min_ratio) hz[i - 1] = hz[i] / min_ratio;
    }
}

class LrBank {
public:
    // bands in 2..kMaxCrossoverBands, crossover_hz has bands - 1 ascending
    // entries (order_crossovers). Not real-time safe only in that it
    // computes tan(); it allocates nothing.
    void prepare(double sample_rate, std::uint32_t bands, const double* crossover_hz) noexcept {
        bands_ = bands < 2 ? 2 : (bands > kMaxCrossoverBands ? kMaxCrossoverBands : bands);
        for (std::uint32_t s = 0; s + 1 < bands_; ++s) {
            const double f = crossover_hz[s] < 1.0 ? 1.0 : crossover_hz[s];
            const double g = std::tan(3.14159265358979323846 * f / sample_rate);
            Coef& c = coef_[s];
            c.k = 1.4142135623730951;  // Butterworth, Q = 1 / sqrt(2)
            c.a1 = 1.0 / (1.0 + g * (g + c.k));
            c.a2 = g * c.a1;
            c.a3 = g * c.a2;
        }
        reset();
    }
    void reset() noexcept {
        for (auto& ch : st_) ch = ChannelState{};
    }
    std::uint32_t bands() const noexcept { return bands_; }

    // One sample of one channel into bands() band samples.
    void process(std::uint32_t channel, double x, double* out) noexcept SONORA_RT {
        ChannelState& st = st_[channel];
        double cur = x;
        const std::uint32_t nx = bands_ - 1;
        for (std::uint32_t s = 0; s < nx; ++s) {
            const Coef& c = coef_[s];
            double v1, v2;
            tick(c, st.first[s], cur, v1, v2);
            const double lp1 = v2;
            const double hp1 = cur - c.k * v1 - v2;
            double w1, w2;
            tick(c, st.lo[s], lp1, w1, w2);
            const double lo = w2;
            tick(c, st.hi[s], hp1, w1, w2);
            const double hi = hp1 - c.k * w1 - w2;
            // The bands already split get this crossover's all-pass, 2nd
            // order: x - 2 k v1 (LP - k BP + HP of the same section).
            for (std::uint32_t j = 0; j < s; ++j) {
                double a1, a2;
                tick(c, st.ap[j][s], out[j], a1, a2);
                out[j] = out[j] - 2.0 * c.k * a1;
            }
            out[s] = lo;
            cur = hi;
        }
        out[nx] = cur;
    }

private:
    struct Coef {
        double k = 1.4142135623730951, a1 = 1.0, a2 = 0.0, a3 = 0.0;
    };
    struct Tpt {
        double ic1 = 0.0, ic2 = 0.0;
    };
    struct ChannelState {
        Tpt first[kMaxCrossoverBands - 1];
        Tpt lo[kMaxCrossoverBands - 1];
        Tpt hi[kMaxCrossoverBands - 1];
        Tpt ap[kMaxCrossoverBands - 1][kMaxCrossoverBands - 1];
    };
    // Zavalishin's trapezoidal state-variable filter: v1 is the band-pass,
    // v2 the low-pass; the high-pass is v0 - k v1 - v2.
    static void tick(const Coef& c, Tpt& s, double v0, double& v1, double& v2) noexcept SONORA_RT {
        const double v3 = v0 - s.ic2;
        v1 = c.a1 * s.ic1 + c.a2 * v3;
        v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = 2.0 * v1 - s.ic1;
        s.ic2 = 2.0 * v2 - s.ic2;
    }

    std::uint32_t bands_ = 2;
    Coef coef_[kMaxCrossoverBands - 1];
    ChannelState st_[kMaxCrossoverChannels];
};

// h[0 .. taps): a Kaiser-windowed-sinc low-pass at fc_hz, unit gain at DC.
// taps odd. Not real-time safe (it computes a Bessel function per tap).
inline void linear_phase_lowpass(double* h, std::uint32_t taps, double fc_hz, double sample_rate,
                                 double kaiser_beta_value) noexcept {
    const double pi = 3.14159265358979323846;
    const double m = 0.5 * static_cast<double>(taps - 1);
    const double wc = 2.0 * pi * fc_hz / sample_rate;
    double sum = 0.0;
    for (std::uint32_t n = 0; n < taps; ++n) {
        const double t = static_cast<double>(n) - m;
        const double sinc = t == 0.0 ? wc / pi : std::sin(wc * t) / (pi * t);
        h[n] = sinc * kaiser(m > 0.0 ? t / m : 0.0, kaiser_beta_value);
        sum += h[n];
    }
    for (std::uint32_t n = 0; n < taps; ++n) h[n] /= sum;
}

}  // namespace sonora::dsp
