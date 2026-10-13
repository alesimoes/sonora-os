// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/true_peak.h"

#include <cmath>

#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"

namespace sonora::dsp {

void TruePeakDetector::prepare(std::uint32_t channels, std::uint32_t taps_per_phase,
                               double stopband_db, bool refine) {
    refine_ = refine;
    taps_ = taps_per_phase < 2 ? 2 : taps_per_phase & ~1u;
    const std::uint32_t n = kFactor * taps_ + 1;
    const double c = (n - 1) / 2.0;
    const double beta = kaiser_beta(stopband_db);
    std::vector<double> h(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const double d = (i - c) / kFactor;
        const double sinc = d == 0.0 ? 1.0 : std::sin(kPi * d) / (kPi * d);
        h[i] = sinc * kaiser((i - c) / c, beta);
    }
    // Each fractional phase is normalised to unit DC gain, so a constant
    // reads the same at every interpolated point.
    for (std::uint32_t p = 1; p < kFactor; ++p) {
        phase_[p].assign(taps_, 0.0);
        double sum = 0.0;
        for (std::uint32_t i = 0; i < taps_; ++i) {
            phase_[p][i] = h[p + kFactor * i];
            sum += phase_[p][i];
        }
        for (double& t : phase_[p]) t /= sum;
    }
    hist_.assign(channels, Hist{});
    for (Hist& hs : hist_) hs.x.assign(2 * taps_, 0.0);
}

void TruePeakDetector::reset() noexcept {
    for (Hist& hs : hist_) {
        for (double& v : hs.x) v = 0.0;
        hs.pos = 0;
        hs.prev2 = hs.prev3 = 0.0;
    }
}

void TruePeakDetector::process_points(std::uint32_t ch, double x, double* out) noexcept {
    Hist& hs = hist_[ch];
    hs.pos = (hs.pos == 0 ? taps_ : hs.pos) - 1;
    hs.x[hs.pos] = hs.x[hs.pos + taps_] = x;
    const double* v = &hs.x[hs.pos];  // v[i] = x[n - i]
    // y[4n + p] = sum_i h[p + 4i] x[n - i]; its time is n - D + p/4 after
    // the filter's delay of 2 T high-rate samples = T / 2 base samples.
    out[0] = v[delay()];
    // Four independent accumulators per phase: a single running sum is
    // bound by the adder's latency. taps_ is even, so a multiple of 2;
    // the tail loop takes the rest. Fixed order: the same on every compiler.
    for (std::uint32_t p = 1; p < kFactor; ++p) {
        const double* h = phase_[p].data();
        double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        std::uint32_t i = 0;
        for (; i + 4 <= taps_; i += 4) {
            a0 += h[i] * v[i];
            a1 += h[i + 1] * v[i + 1];
            a2 += h[i + 2] * v[i + 2];
            a3 += h[i + 3] * v[i + 3];
        }
        for (; i < taps_; ++i) a0 += h[i] * v[i];
        out[p] = (a0 + a1) + (a2 + a3);
    }
}

double TruePeakDetector::peak_of(std::uint32_t ch, const double* pts) noexcept {
    double y[kFactor + 2];  // signed: two points of the last sample, then these
    Hist& hs = hist_[ch];
    y[0] = hs.prev2;
    y[1] = hs.prev3;
    double m = 0.0;
    for (std::uint32_t p = 0; p < kFactor; ++p) {
        y[p + 2] = pts[p];
        const double a = std::fabs(pts[p]);
        if (a > m) m = a;
    }
    hs.prev2 = y[kFactor];
    hs.prev3 = y[kFactor + 1];
    if (!refine_) return m;
    // Around each local maximum of |y|, fit the sinusoid through the three
    // equally spaced points: with u = y * sign(y1), cos h = (u0 + u2) / 2 u1
    // and the amplitude is sqrt(u1^2 + ((u2 - u0) / (2 sin h))^2), exact for
    // a sinusoid at any frequency. A parabola through the same points is
    // 0.05 dB low at 0.45 fs, which the 16x reference finds as an over.
    for (std::uint32_t c = 1; c <= kFactor; ++c) {
        const double s = y[c] < 0.0 ? -1.0 : 1.0;
        const double u0 = s * y[c - 1], u1 = s * y[c], u2 = s * y[c + 1];
        if (u1 <= 0.0 || u1 < std::fabs(u0) || u1 < std::fabs(u2)) continue;
        const double cs = (u0 + u2) / (2.0 * u1);
        double v = u1;
        if (cs > -1.0 && cs < 1.0) {
            const double sn = std::sqrt(1.0 - cs * cs);
            const double d = (u2 - u0) / (2.0 * sn);
            v = std::sqrt(u1 * u1 + d * d);
        }
        // Content below Nyquist cannot peak more than 1 / cos(pi / 8) above
        // a 4x point; a larger estimate comes from noise in the fit.
        const double cap = 1.0824 * u1;
        if (v > cap) v = cap;
        if (v > m) m = v;
    }
    return m;
}

double TruePeakDetector::process(std::uint32_t ch, double x, double* sample_peak) noexcept {
    double pts[kFactor];
    process_points(ch, x, pts);
    if (sample_peak != nullptr) *sample_peak = std::fabs(pts[0]);
    return peak_of(ch, pts);
}

}  // namespace sonora::dsp
