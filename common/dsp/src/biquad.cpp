// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/biquad.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

AnalogSection analog_prototype(const FilterSpec& spec) noexcept {
    // The s-domain forms of the RBJ cookbook, with A = 10^(gain/40).
    const double q = spec.q > 1e-6 ? spec.q : 1e-6;
    const double a = std::pow(10.0, spec.gain_db / 40.0);
    const double sa = std::sqrt(a);
    AnalogSection s;
    switch (spec.shape) {
        case Shape::Peak:
            s = {1.0, a / q, 1.0, 1.0, 1.0 / (a * q), 1.0};
            break;
        case Shape::LowShelf:
            s = {a, a * sa / q, a * a, a, sa / q, 1.0};
            break;
        case Shape::HighShelf:
            s = {a * a, a * sa / q, a, 1.0, sa / q, a};
            break;
        case Shape::LowPass:
            s = {0.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0};
            break;
        case Shape::HighPass:
            s = {1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0};
            break;
        case Shape::BandPass:
            s = {0.0, 1.0 / q, 0.0, 1.0, 1.0 / q, 1.0};
            break;
        case Shape::Notch:
            s = {1.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0};
            break;
        case Shape::LowPass1:
            s = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0};
            break;
        case Shape::HighPass1:
            s = {0.0, 1.0, 0.0, 0.0, 1.0, 1.0};
            break;
    }
    return s;
}

namespace {

std::complex<double> eval_section(const AnalogSection& s, double w) noexcept {
    const std::complex<double> jw(0.0, w);
    const std::complex<double> num = (s.n2 * jw + s.n1) * jw + s.n0;
    const std::complex<double> den = (s.d2 * jw + s.d1) * jw + s.d0;
    return num / den;
}

double mag2_at(const AnalogSection& s, double w) noexcept {
    return std::norm(eval_section(s, w));
}


}  // namespace

std::complex<double> analog_response(const FilterSpec& spec, double f_hz) noexcept {
    return eval_section(analog_prototype(spec), f_hz / spec.freq_hz);
}

std::complex<double> response(const BiquadCoeffs& c, double f_hz,
                              double sample_rate) noexcept {
    const double w = kTwoPi * f_hz / sample_rate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

BiquadCoeffs bilinear(const AnalogSection& s, double f0_hz,
                      double sample_rate) noexcept {
    // s -> (1/K) (z-1)/(z+1) with K = tan(pi f0 / fs), multiplied through
    // by K^2 (z+1)^2.
    const double k = std::tan(kPi * f0_hz / sample_rate);
    const double k2 = k * k;
    const double nb0 = s.n2 + s.n1 * k + s.n0 * k2;
    const double nb1 = 2.0 * (s.n0 * k2 - s.n2);
    const double nb2 = s.n2 - s.n1 * k + s.n0 * k2;
    const double da0 = s.d2 + s.d1 * k + s.d0 * k2;
    const double da1 = 2.0 * (s.d0 * k2 - s.d2);
    const double da2 = s.d2 - s.d1 * k + s.d0 * k2;
    BiquadCoeffs c;
    c.b0 = nb0 / da0;
    c.b1 = nb1 / da0;
    c.b2 = nb2 / da0;
    c.a1 = da1 / da0;
    c.a2 = da2 / da0;
    return c;
}

BiquadCoeffs design_bilinear(const FilterSpec& spec, double sample_rate) noexcept {
    const double f = clamp(spec.freq_hz, 1e-3, kMaxFreqRatio * sample_rate);
    return bilinear(analog_prototype(spec), f, sample_rate);
}

namespace {

// Vicanek 2016, eqs. (25)-(27): with
//   phi0 = 1 - sin^2(w/2), phi1 = sin^2(w/2), phi2 = 4 phi0 phi1,
// |b0 + b1 z^-1 + b2 z^-2|^2 = B0 phi0 + B1 phi1 + B2 phi2, where
//   B0 = (b0+b1+b2)^2, B1 = (b0-b1+b2)^2, B2 = -4 b0 b2,
// and the same for the denominator. Magnitude conditions are therefore
// linear in (B0, B1, B2).
struct Phi {
    double p0, p1, p2;
};
Phi phi(double w) noexcept {
    const double s = std::sin(0.5 * w);
    const double p1 = s * s;
    const double p0 = 1.0 - p1;
    return {p0, p1, 4.0 * p0 * p1};
}
double dot(const double* a, const Phi& p) noexcept {
    return a[0] * p.p0 + a[1] * p.p1 + a[2] * p.p2;
}

struct Poles {
    double a1, a2;
    double A[3];  // A0, A1, A2
};

// Vicanek eq. (12): the analog pole pair mapped by z = e^{sT}. The
// analog denominator d2 s^2 + d1 s + d0 (s in units of w0) gives the
// natural frequency and damping.
Poles matched_poles(const AnalogSection& s, double w0) noexcept {
    const double wn = std::sqrt(s.d0 / s.d2);
    const double zeta = s.d1 / (2.0 * s.d2 * wn);
    const double wp = w0 * wn;
    const double r = std::exp(-zeta * wp);
    Poles p;
    if (zeta <= 1.0) {
        p.a1 = -2.0 * r * std::cos(wp * std::sqrt(1.0 - zeta * zeta));
    } else {
        p.a1 = -2.0 * r * std::cosh(wp * std::sqrt(zeta * zeta - 1.0));
    }
    p.a2 = std::exp(-2.0 * zeta * wp);
    p.A[0] = (1.0 + p.a1 + p.a2) * (1.0 + p.a1 + p.a2);
    p.A[1] = (1.0 - p.a1 + p.a2) * (1.0 - p.a1 + p.a2);
    p.A[2] = -4.0 * p.a2;
    return p;
}

// Vicanek eq. (29): (B0, B1, B2) -> (b0, b1, b2), choosing the root with
// the zeros inside the unit circle, so every section is minimum-phase like
// its analog prototype and the reciprocal sections below are stable.
BiquadCoeffs from_squares(double B0, double B1, double B2, const Poles& p) noexcept {
    const double sq0 = std::sqrt(B0 > 0.0 ? B0 : 0.0);
    const double sq1 = std::sqrt(B1 > 0.0 ? B1 : 0.0);
    const double w = 0.5 * (sq0 + sq1);
    const double disc = w * w + B2;
    BiquadCoeffs c;
    c.b0 = 0.5 * (w + std::sqrt(disc > 0.0 ? disc : 0.0));
    c.b1 = 0.5 * (sq0 - sq1);
    c.b2 = c.b0 != 0.0 ? -B2 / (4.0 * c.b0) : 0.0;
    c.a1 = p.a1;
    c.a2 = p.a2;
    return c;
}

// Numerator matching the analog magnitude at three frequencies (Vicanek
// §4 notes this route; the published shapes below use slope conditions
// instead where they have one).
BiquadCoeffs match_three(const AnalogSection& s, double w0, const Poles& p,
                         const double w[3]) noexcept {
    double m[3][4];
    for (int i = 0; i < 3; ++i) {
        const Phi f = phi(w[i]);
        m[i][0] = f.p0;
        m[i][1] = f.p1;
        m[i][2] = f.p2;
        m[i][3] = mag2_at(s, w[i] / w0) * dot(p.A, f);
    }
    // Gaussian elimination with partial pivoting on the 3x4 system.
    for (int c = 0; c < 3; ++c) {
        int piv = c;
        for (int r = c + 1; r < 3; ++r) {
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        }
        for (int k = 0; k < 4; ++k) {
            const double t = m[c][k];
            m[c][k] = m[piv][k];
            m[piv][k] = t;
        }
        for (int r = c + 1; r < 3; ++r) {
            const double f = m[r][c] / m[c][c];
            for (int k = c; k < 4; ++k) m[r][k] -= f * m[c][k];
        }
    }
    double B[3];
    for (int r = 2; r >= 0; --r) {
        double acc = m[r][3];
        for (int k = r + 1; k < 3; ++k) acc -= m[r][k] * B[k];
        B[r] = acc / m[r][r];
    }
    return from_squares(B[0], B[1], B[2], p);
}

// Vicanek §4.4, the matched peaking EQ for a boost: unity at DC, the
// prototype's gain at w0, and a maximum at w0 (eqs. 43-45).
BiquadCoeffs peak_boost(const AnalogSection& s, double w0, double g2) noexcept {
    const Poles p = matched_poles(s, w0);
    const Phi f = phi(w0);
    const double B0 = p.A[0];
    const double R1 = dot(p.A, f) * g2;
    const double R2 = (-p.A[0] + p.A[1] + 4.0 * (f.p0 - f.p1) * p.A[2]) * g2;
    const double B2 = (R1 - R2 * f.p1 - B0) / (4.0 * f.p1 * f.p1);
    const double B1 = R2 + B0 + 4.0 * (f.p1 - f.p0) * B2;
    return from_squares(B0, B1, B2, p);
}

// Vicanek §4.1: single zero (b2 = 0), unity at DC, the prototype's
// magnitude (Q) at w0 (eqs. 31-33).
BiquadCoeffs low_pass(const AnalogSection& s, double w0) noexcept {
    const Poles p = matched_poles(s, w0);
    const Phi f = phi(w0);
    const double B0 = p.A[0];
    double B1 = (dot(p.A, f) * mag2_at(s, 1.0) - B0 * f.p0) / f.p1;
    if (B1 < 0.0) B1 = 0.0;
    BiquadCoeffs c;
    c.b0 = 0.5 * (std::sqrt(B0) + std::sqrt(B1));
    c.b1 = std::sqrt(B0) - c.b0;
    c.b2 = 0.0;
    c.a1 = p.a1;
    c.a2 = p.a2;
    return c;
}

// Vicanek §4.2: double zero at DC, the prototype's magnitude at w0 (eq. 36).
BiquadCoeffs high_pass(const AnalogSection& s, double w0) noexcept {
    const Poles p = matched_poles(s, w0);
    const Phi f = phi(w0);
    BiquadCoeffs c;
    c.b0 = std::sqrt(dot(p.A, f) * mag2_at(s, 1.0)) / (4.0 * f.p1);
    c.b1 = -2.0 * c.b0;
    c.b2 = c.b0;
    c.a1 = p.a1;
    c.a2 = p.a2;
    return c;
}

// Vicanek §4.3: single zero at DC, unity maximum at w0 (eqs. 38-41).
BiquadCoeffs band_pass(const AnalogSection& s, double w0) noexcept {
    const Poles p = matched_poles(s, w0);
    const Phi f = phi(w0);
    const double R1 = dot(p.A, f);
    const double R2 = -p.A[0] + p.A[1] + 4.0 * (f.p0 - f.p1) * p.A[2];
    const double B2 = (R1 - R2 * f.p1) / (4.0 * f.p1 * f.p1);
    const double B1 = R2 + 4.0 * (f.p1 - f.p0) * B2;
    BiquadCoeffs c;
    c.b1 = -0.5 * std::sqrt(B1 > 0.0 ? B1 : 0.0);
    const double d = B2 + c.b1 * c.b1;
    c.b0 = 0.5 * (std::sqrt(d > 0.0 ? d : 0.0) - c.b1);
    c.b2 = -c.b0 - c.b1;
    c.a1 = p.a1;
    c.a2 = p.a2;
    return c;
}

// Shelves are not in the paper. Ours: matched poles, numerator matching
// the prototype at DC, at f0 (the half-gain point) and at 2 f0 (or half
// way to Nyquist when 2 f0 is past it). Measured against the analog
// shelf in tests/test_filters.cpp; matching DC, f0 and Nyquist instead is
// up to ten times worse at 88.2 kHz.
BiquadCoeffs shelf_rising_poles_low(const AnalogSection& s, double w0) noexcept {
    const Poles p = matched_poles(s, w0);
    double hi = 2.0 * w0;
    if (hi > 0.99 * kPi) hi = 0.5 * (w0 + kPi);
    const double w[3] = {0.0, w0, hi};
    return match_three(s, w0, p, w);
}

BiquadCoeffs notch(const AnalogSection& s, double w0) noexcept {
    const Poles p = matched_poles(s, w0);
    const double w[3] = {0.0, kPi, w0};
    return match_three(s, w0, p, w);
}

// Vicanek's one-pole counterpart: matched pole, numerator matching DC and
// w0 (the -3 dB point). |b0 + b1 z^-1|^2 = B0 phi0 + B1 phi1.
BiquadCoeffs first_order(const AnalogSection& s, double w0) noexcept {
    const double a1 = -std::exp(-w0 * s.d0 / s.d1);
    const double A0 = (1.0 + a1) * (1.0 + a1);
    const double A1 = (1.0 - a1) * (1.0 - a1);
    const Phi f = phi(w0);
    const double B0 = mag2_at(s, 0.0) * A0;
    double B1 = (mag2_at(s, 1.0) * (A0 * f.p0 + A1 * f.p1) - B0 * f.p0) / f.p1;
    if (B1 < 0.0) B1 = 0.0;
    BiquadCoeffs c;
    c.b0 = 0.5 * (std::sqrt(B0) + std::sqrt(B1));
    c.b1 = 0.5 * (std::sqrt(B0) - std::sqrt(B1));
    c.b2 = 0.0;
    c.a1 = a1;
    c.a2 = 0.0;
    return c;
}

// 1/H for a section whose zeros are inside the unit circle.
BiquadCoeffs invert(const BiquadCoeffs& c) noexcept {
    BiquadCoeffs r;
    r.b0 = 1.0 / c.b0;
    r.b1 = c.a1 / c.b0;
    r.b2 = c.a2 / c.b0;
    r.a1 = c.b1 / c.b0;
    r.a2 = c.b2 / c.b0;
    return r;
}

}  // namespace

BiquadCoeffs design_matched(const FilterSpec& spec, double sample_rate) noexcept {
    FilterSpec sp = spec;
    sp.freq_hz = clamp(spec.freq_hz, 1e-3, kMaxFreqRatio * sample_rate);
    const double w0 = kTwoPi * sp.freq_hz / sample_rate;

    switch (sp.shape) {
        case Shape::LowPass1:
        case Shape::HighPass1:
            return first_order(analog_prototype(sp), w0);
        case Shape::LowPass:
            return low_pass(analog_prototype(sp), w0);
        case Shape::HighPass:
            return high_pass(analog_prototype(sp), w0);
        case Shape::BandPass:
            return band_pass(analog_prototype(sp), w0);
        case Shape::Notch:
            return notch(analog_prototype(sp), w0);
        case Shape::Peak:
        case Shape::LowShelf:
        case Shape::HighShelf:
            break;
    }
    // A bell cut, a low-shelf cut and a high-shelf boost are the exact
    // reciprocals (A -> 1/A) of a bell boost, a low-shelf boost and a
    // high-shelf cut. Those have the sharper poles, below f0, where matched
    // poles are most accurate; design them and invert.
    const bool flip = (sp.shape == Shape::LowShelf && sp.gain_db < 0.0) ||
                      (sp.shape == Shape::HighShelf && sp.gain_db > 0.0) ||
                      (sp.shape == Shape::Peak && sp.gain_db < 0.0);
    if (flip) sp.gain_db = -sp.gain_db;
    const AnalogSection s = analog_prototype(sp);
    BiquadCoeffs c;
    if (sp.shape == Shape::Peak) {
        c = peak_boost(s, w0, std::pow(10.0, sp.gain_db / 10.0));
    } else {
        c = shelf_rising_poles_low(s, w0);
    }
    return flip ? invert(c) : c;
}

SvfCoeffs to_svf(const BiquadCoeffs& c) noexcept {
    // The trapezoidal SVF's denominator is the bilinear image of
    // s^2 + k s + 1 with s = (1/g)(z-1)/(z+1):
    //   1 + a1 + a2 = 4 g^2 / D,  1 - a1 + a2 = 4 / D,  D = 1 + g k + g^2.
    // Its outputs are v0 (input), v1 (band-pass s/(s^2+ks+1)) and v2
    // (low-pass 1/(s^2+ks+1)); matching the numerator at z = -1, z = 1 and
    // in b0 - b2 gives m0, m2 and m1.
    const double sum = 1.0 + c.a1 + c.a2;
    const double dif = 1.0 - c.a1 + c.a2;
    SvfCoeffs s;
    double g2 = sum / dif;
    if (!(g2 > 1e-24)) g2 = 1e-24;
    s.g = std::sqrt(g2);
    const double d = 4.0 / dif;
    s.k = (d - 1.0 - g2) / s.g;
    s.m0 = d * (c.b0 - c.b1 + c.b2) / 4.0;
    s.m2 = d * (c.b0 + c.b1 + c.b2) / (4.0 * g2) - s.m0;
    s.m1 = (d * (c.b0 - c.b2) - 2.0 * s.m0 * s.g * s.k) / (2.0 * s.g);
    return s;
}

int butterworth_sections(int order, bool high_pass, double freq_hz,
                         FilterSpec* out) noexcept {
    order = clamp(order, 1, 8);
    int n = 0;
    for (int k = 1; k <= order / 2; ++k) {
        // Angle of the k-th pole pair from the negative real axis.
        const double theta = order % 2 == 0 ? (2.0 * k - 1.0) * kPi / (2.0 * order)
                                            : k * kPi / order;
        FilterSpec s;
        s.shape = high_pass ? Shape::HighPass : Shape::LowPass;
        s.freq_hz = freq_hz;
        s.q = 1.0 / (2.0 * std::cos(theta));
        out[n++] = s;
    }
    if (order % 2 == 1) {
        FilterSpec s;
        s.shape = high_pass ? Shape::HighPass1 : Shape::LowPass1;
        s.freq_hz = freq_hz;
        out[n++] = s;
    }
    return n;
}

}  // namespace sonora::dsp
