// SPDX-License-Identifier: GPL-3.0-or-later
//
// Magnetic tape, record and playback (ADR-0011 §6, N2 slice 4).
//
// The nonlinearity is the Jiles-Atherton model of ferromagnetic
// hysteresis: D. C. Jiles and D. L. Atherton, "Theory of ferromagnetic
// hysteresis", J. Magn. Magn. Mater. 61, 1986, in the form of Jiles,
// "Numerical determination of hysteresis parameters for the modeling of
// magnetic properties using the theory of ferromagnetic hysteresis", IEEE
// Trans. Magn. 28(1), 1992. State is the magnetisation M; the field H is
// the input. With
//   He   = H + alpha M                       (the effective field)
//   Man  = Ms L(He / a)                      (anhysteretic, L the Langevin function)
//   Mirr = (M - c Man) / (1 - c)             (M = c Man + (1 - c) Mirr)
//   dMirr/dHe = max(0, delta (Man - Mirr)) / k,   delta = sign(dH)
// the susceptibility is
//   A = (1 - c) dMirr/dHe + c dMan/dHe,      dM/dH = A / (1 - alpha A).
// The `max(0, .)` is the standard guard against a negative irreversible
// susceptibility (D. C. Jiles, J. B. Thoelke, M. K. Devine, IEEE Trans.
// Magn. 28(1), 1992). Applying it to audio follows J. Chowdhury,
// "Real-time physical modelling for analog tape machines", DAFx-19, 2019:
// the same equations, integrated per sample, with the sign of the field's
// step as delta. The integrator here is classical RK4 over a number of
// substeps chosen from the field's step against the stiffness k, so the
// error does not depend on how hard the signal is driven; it is bounded
// (kMaxSubsteps), so the cost per sample is too.
//
// Biased tape. A recorder adds a high-frequency bias so that the tape
// follows the anhysteretic curve (W. K. Westmijze, 1953; R. Mallinson, "The
// Magnetic Recording Handbook", 1990, ch. 4), not the loop: the parameter
// set below is a small k and a large c, which is that regime: a Langevin
// compression, an odd-harmonic series, and only a thin loop. These are
// modelling values, chosen for the regime and not measured on a machine;
// nothing here is fitted to a product.
//
// Equalisation. The record and playback time constants are NAB (7.5 and
// 15 in/s: 50 us) and IEC (7.5 in/s: 70 us, 15 in/s: 35 us), and 17.5 us
// at 30 in/s (AES). The 3180 us part of the NAB playback curve compensates
// the head's differentiation of flux; this model has no differentiation,
// so it is absent, and the pre-emphasis in front of the tape is
// (1 + s tau) / (1 + s tau_limit) with its exact digital inverse behind it:
// the linear response of record then playback is flat by construction and
// the tape's saturation sees the high frequencies first, as it does.
//
// Playback head. The head's gap and the spacing between head and tape
// attenuate short wavelengths by
//   L(f) = sinc(pi f g / v) exp(-2 pi f d / v)
// (R. L. Wallace Jr., "The reproduction of magnetically recorded signals",
// Bell Syst. Tech. J. 30, 1951, for the gap term; the spacing term is the
// Wallace spacing loss, 54.6 d / lambda dB). A one-pole low-pass whose
// corner minimises the largest error against L over 20 Hz to 20 kHz stands
// in for it; its error is reported by the suite.
//
// Head bump. The playback head's finite length gives a low-frequency
// ripple at the frequencies where the head length is a whole number of
// wavelengths (Mallinson, ch. 5): f = v / lambda_b, with lambda_b a
// modelling value, 5 mm. Added as a band-pass in parallel with the
// signal, so its amount is a linear gain and moving it cannot zipper.
//
// Real-time: nothing here allocates; every function is bounded arithmetic.
#pragma once

#include <cmath>
#include <cstdint>

#include "sonora/dsp/math.h"
#include "sonora/rt.h"

namespace sonora::dsp {

struct JilesAthertonParams {
    double ms = 1.0;      // saturation magnetisation (units of M)
    double a = 1.0;       // anhysteretic shape, in units of H
    double k = 0.45;      // pinning: loop width, in units of H
    double c = 0.8;       // reversibility (1: anhysteretic, no loop)
    double alpha = 0.2;   // domain coupling (the mean field)
};

// The Langevin function L(x) = coth x - 1/x and its slope, with the series
// near zero where the closed forms cancel.
inline double langevin(double x) noexcept SONORA_RT {
    const double ax = std::fabs(x);
    if (ax < 0.25) {
        const double x2 = x * x;
        return x * (1.0 / 3.0 + x2 * (-1.0 / 45.0 + x2 * (2.0 / 945.0 + x2 * (-1.0 / 4725.0 + 2.0 * x2 / 93555.0))));
    }
    // coth x = 1 / tanh x; beyond 20 it is 1 to double precision.
    if (ax > 20.0) return (x > 0.0 ? 1.0 : -1.0) - 1.0 / x;
    return 1.0 / std::tanh(x) - 1.0 / x;
}
inline double langevin_slope(double x) noexcept SONORA_RT {
    const double ax = std::fabs(x);
    if (ax < 0.25) {
        const double x2 = x * x;
        return 1.0 / 3.0 + x2 * (-1.0 / 15.0 + x2 * (2.0 / 189.0 + x2 * (-1.0 / 675.0 + 2.0 * x2 / 10395.0)));
    }
    if (ax > 20.0) return 1.0 / (x * x);
    const double t = std::tanh(x);
    return 1.0 / (x * x) - (1.0 - t * t) / (t * t);
}

class TapeHysteresis {
public:
    static constexpr std::uint32_t kMaxSubsteps = 32;

    void set_params(const JilesAthertonParams& p) noexcept {
        p_ = p;
        sub_h_ = 0.8 * p.k;
    }
    const JilesAthertonParams& params() const noexcept { return p_; }
    void reset() noexcept {
        m_ = 0.0;
        h_ = 0.0;
    }
    double magnetisation() const noexcept { return m_; }

    // Susceptibility dM/dH at M = 0, the small-signal slope the
    // processor divides out: the irreversible part is zero there
    // (Man = Mirr = 0).
    double initial_susceptibility() const noexcept {
        const double s = p_.c * p_.ms / p_.a / 3.0;
        return s / (1.0 - p_.alpha * s);
    }

    // The field moves to `h` over one sample; returns M. The substep count
    // follows the step against the pinning stiffness.
    double tick(double h) noexcept SONORA_RT {
        const double dh = h - h_;
        double n = std::ceil(std::fabs(dh) / sub_h_);
        if (!(n >= 1.0)) n = 1.0;
        if (n > static_cast<double>(kMaxSubsteps)) n = static_cast<double>(kMaxSubsteps);
        return tick_with(h, static_cast<std::uint32_t>(n));
    }
    // The same with a given substep count: the reference render of the
    // tests uses hundreds.
    double tick_with(double h, std::uint32_t substeps) noexcept SONORA_RT {
        const double dh = h - h_;
        if (dh != 0.0) {
            const double delta = dh > 0.0 ? 1.0 : -1.0;
            const double step = dh / static_cast<double>(substeps);
            double hh = h_, m = m_;
            for (std::uint32_t i = 0; i < substeps; ++i) {
                const double k1 = slope(hh, m, delta);
                const double k2 = slope(hh + 0.5 * step, m + 0.5 * step * k1, delta);
                const double k3 = slope(hh + 0.5 * step, m + 0.5 * step * k2, delta);
                const double k4 = slope(hh + step, m + step * k3, delta);
                m += step * (k1 + 2.0 * k2 + 2.0 * k3 + k4) * (1.0 / 6.0);
                hh += step;
                if (m > p_.ms) m = p_.ms;
                if (m < -p_.ms) m = -p_.ms;
            }
            m_ = m;
        }
        h_ = h;
        return m_;
    }

    // dM/dH at (H, M) for a rising (delta = 1) or falling field.
    double slope(double h, double m, double delta) const noexcept SONORA_RT {
        const double he = h + p_.alpha * m;
        const double x = he / p_.a;
        const double man = p_.ms * langevin(x);
        const double dman = p_.ms / p_.a * langevin_slope(x);
        const double mirr = (m - p_.c * man) / (1.0 - p_.c);
        double dmirr = delta * (man - mirr);
        dmirr = dmirr > 0.0 ? dmirr / p_.k : 0.0;
        const double a = (1.0 - p_.c) * dmirr + p_.c * dman;
        double den = 1.0 - p_.alpha * a;
        if (den < 0.05) den = 0.05;
        return a / den;
    }

private:
    JilesAthertonParams p_;
    double sub_h_ = 0.36;
    double m_ = 0.0;
    double h_ = 0.0;
};

// y[n] = b0 x[n] + b1 x[n-1] + p y[n-1].
struct OnePoleZero {
    double b0 = 1.0, b1 = 0.0, p = 0.0;
    double x1 = 0.0, y1 = 0.0;

    double tick(double x) noexcept SONORA_RT {
        const double y = b0 * x + b1 * x1 + p * y1;
        x1 = x;
        y1 = y;
        return y;
    }
    void reset() noexcept { x1 = y1 = 0.0; }
};

// The digital counterpart of (1 + s / wz) / (1 + s / wp), unity at DC
// (wz = 0 Hz means no zero): the pole by the matched-z transform and the
// numerator so the magnitude equals the analog one at DC and at Nyquist,
// the first-order case of M. Vicanek, "Matched Second Order Digital
// Filters", 2016 (biquad.h). No cramping at Nyquist, which a bilinear
// design has, and a stable exact inverse (tape_invert).
OnePoleZero design_matched_one_pole(double zero_hz, double pole_hz, double sample_rate) noexcept;
// The exact inverse: (1 - p z^-1) / (b0 + b1 z^-1). Stable and causal
// where the zero of the forward filter is inside the unit circle, which
// the matched design guarantees.
struct OnePoleInverse {
    double ib0 = 1.0, b1 = 0.0, p = 0.0;  // y[n] = (x[n] - p x[n-1] - b1 y[n-1]) / b0
    double x1 = 0.0, y1 = 0.0;

    double tick(double x) noexcept SONORA_RT {
        const double y = ib0 * (x - p * x1 - b1 * y1);
        x1 = x;
        y1 = y;
        return y;
    }
    void reset() noexcept { x1 = y1 = 0.0; }
};
OnePoleInverse invert(const OnePoleZero& f) noexcept;
// |H(f)| of the digital filter, linear.
double one_pole_zero_magnitude(const OnePoleZero& f, double f_hz, double sample_rate) noexcept;

enum class TapeSpeed : std::uint8_t { Ips7_5 = 0, Ips15 = 1, Ips30 = 2 };
enum class TapeEq : std::uint8_t { Nab = 0, Iec = 1 };

// Tape speed in metres per second.
double tape_speed_mps(TapeSpeed s) noexcept;
// The pre-emphasis time constant in seconds (see the header).
double tape_eq_tau(TapeSpeed s, TapeEq e) noexcept;
// The pre-emphasis' limit pole, above the audio band.
constexpr double kTapePreemphasisLimitHz = 24000.0;

// The playback head's modelling values: gap and spacing, in metres.
constexpr double kTapeGapM = 2e-6;
constexpr double kTapeSpacingM = 0.5e-6;
// |L(f)|, linear.
double tape_head_loss(double f_hz, TapeSpeed s) noexcept;
// The corner of the one-pole low-pass that minimises the largest dB error
// against tape_head_loss over 20 Hz to 20 kHz.
double tape_head_loss_corner_hz(TapeSpeed s) noexcept;
// The head bump's frequency and its band-pass Q; bump_db is the boost at
// 100 % (a typical 2 to 4 dB ripple, Mallinson ch. 5).
double tape_bump_hz(TapeSpeed s) noexcept;
constexpr double kTapeBumpQ = 1.0;
constexpr double kTapeBumpMaxDb = 3.0;
constexpr double kTapeBumpLambdaM = 5e-3;

}  // namespace sonora::dsp
