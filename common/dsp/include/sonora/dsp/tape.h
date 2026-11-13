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
// step as delta. The processor's integrator is an exponential integrator
// of the equivalent differential-algebraic form, with a bounded number of
// substeps and a tabulated Langevin function (TapeHysteresis, below);
// classical RK4 of the ODE as written, with the exact Langevin function,
// is kept as the reference the tests hold it to (tick_with()).
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
    double c = 0.8;       // reversibility (1: anhysteretic; kept below, see kMaxReversibility)
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

// The Langevin function for the processor's integrator: on |x| < kMax a
// cubic per cell of width 1/kPerUnit, the Hermite interpolant of L and L'
// at the nodes (so value and slope are continuous across cells), mirrored
// for x < 0 so the curve stays exactly odd; beyond kMax the asymptote
// 1 - 1/x, where coth x is 1 to 2.5e-14. The slope is the cubic's own
// derivative. Built in the constructor; a lookup is a multiply, a
// truncation and a cubic, against a tanh and three divisions for
// langevin(). Its error against langevin() and langevin_slope() is held
// by test_dsp_tape to kValueError and kSlopeError.
class LangevinTable {
public:
    static constexpr double kMax = 16.0;
    static constexpr int kPerUnit = 16;
    static constexpr int kCells = 256;  // kMax * kPerUnit
    // Bounds over 2e6 points of [-20, 20] (test_dsp_tape), with headroom
    // over the measured figures.
    static constexpr double kValueError = 1e-8;
    static constexpr double kSlopeError = 5e-7;

    LangevinTable() noexcept {
        constexpr double h = 1.0 / kPerUnit;
        for (int i = 0; i < kCells; ++i) {
            const double x0 = i * h, x1 = (i + 1) * h;
            const double y0 = langevin(x0), y1 = langevin(x1);
            const double d0 = langevin_slope(x0) * h, d1 = langevin_slope(x1) * h;
            c_[i][0] = y0;
            c_[i][1] = d0;
            c_[i][2] = 3.0 * (y1 - y0) - 2.0 * d0 - d1;
            c_[i][3] = 2.0 * (y0 - y1) + d0 + d1;
        }
    }

    double value(double x) const noexcept SONORA_RT {
        const double ax = std::fabs(x);
        double v;
        if (!(ax < kMax)) {
            v = 1.0 - 1.0 / ax;
        } else {
            const double u = ax * kPerUnit;
            const int i = static_cast<int>(u);
            const double t = u - i;
            const double* c = c_[i];
            v = (c[0] + t * c[1]) + (t * t) * (c[2] + t * c[3]);
        }
        return x < 0.0 ? -v : v;
    }
    // L(x) and L'(x) together, from the same cell.
    void eval(double x, double& l, double& dl) const noexcept SONORA_RT {
        const double ax = std::fabs(x);
        double v;
        if (!(ax < kMax)) {
            const double r = 1.0 / ax;
            v = 1.0 - r;
            dl = r * r;
        } else {
            const double u = ax * kPerUnit;
            const int i = static_cast<int>(u);
            const double t = u - i;
            const double* c = c_[i];
            const double t2 = t * t;
            v = (c[0] + t * c[1]) + t2 * (c[2] + t * c[3]);
            dl = ((c[1] + 2.0 * t * c[2]) + 3.0 * t2 * c[3]) * kPerUnit;
        }
        l = x < 0.0 ? -v : v;
    }

    // Cell i's cubic in t = kPerUnit x - i: c[0] + c[1] t + c[2] t^2 + c[3] t^3.
    const double* cell(int i) const noexcept { return c_[i]; }

private:
    double c_[kCells][4];
};

template <class V, class Bits>
struct TapeSolve;  // src/tape_solve.h

// The processor's integrator. The ODE above is equivalent to the
// differential-algebraic form
//   He = H + alpha M,   M = c Man(He) + (1 - c) Mirr,
//   dMirr/ds = (Man(He) - Mirr) / k   while delta (Man - Mirr) > 0,
//   Mirr held                         otherwise,
// with s = |He - He0| the distance the effective field has travelled
// (He moves with H: dHe/dH = 1 / (1 - alpha A) > 0). Mirr is a first-order
// relaxation towards the anhysteretic curve, linear in Mirr, so over a
// step it is integrated exactly for Man taken as a polynomial in the
// distance: exponential time differencing (S. M. Cox and P. C. Matthews,
// "Exponential time differencing for stiff systems", J. Comput. Phys.
// 176, 2002; M. Hochbruck and A. Ostermann, "Exponential integrators",
// Acta Numerica 19, 2010), here in He rather than in time. With
// sigma = s / D, tau = D / k and Man = sum_j p_j sigma^j over the step,
//   Mirr(D) = exp(-tau) Mirr(0) + sum_j p_j w_j(tau),
//   w_j(tau) = integral over [0, 1] of tau exp(-tau (1 - sigma)) sigma^j,
// w_0 = 1 - exp(-tau), w_j = 1 - (j / tau) w_(j-1). The polynomial is the
// quartic through Man and its slope at both ends and Man halfway (the
// start's are the last step's; the two lookups, at the end and halfway,
// do not wait on each other). Mirr is a weighted mean of its start and of
// Man, so it stays inside the curve's range at any step: no step makes it
// unstable, and none needs the RK4's growing substep count to stay
// bounded. Where Mirr
// leads Man (after a reversal) it holds until Man reaches it, and relaxes
// from there. The end field He1 is implicit (Man and Mirr depend on it):
// one Newton step on F(He1) = H1 + alpha M(He1) - He1, from the step the
// previous susceptibility predicts, applied to the state to first order.
// The substeps take tape_solve.h's code, written once for one instance
// and for two in lockstep.
//
// Accuracy, against tick_with() at 512 RK4 substeps of the exact
// Langevin function: test_dsp_tape and the suite's reference chain;
// brain/research/native-tape-n2.md, "CPU class B", has the figures.
class TapeHysteresis {
public:
    // A substep for every kSubstepH (units of a) the field moves in a
    // sample, up to max_substeps() (at most kMaxSubsteps): the cost per
    // sample is bounded by it.
    static constexpr std::uint32_t kMaxSubsteps = 16;
    static constexpr double kSubstepH = 1.0;

    TapeHysteresis() noexcept { set_params(JilesAthertonParams{}); }

    // The processor's quality chooses it: the substeps a sample may take.
    void set_max_substeps(std::uint32_t n) noexcept {
        max_substeps_ = n < 1 ? 1 : (n > kMaxSubsteps ? kMaxSubsteps : n);
    }
    std::uint32_t max_substeps() const noexcept { return max_substeps_; }

    // The largest reversibility c that set_params keeps. The algebraic form
    // divides by 1 - c (Mirr = (M - c Man) / (1 - c)), which is 0/0 at the
    // anhysteretic limit c = 1: a NaN the clamps let through, so the state
    // never recovered. 1 - 1e-3 keeps that division's rounding amplified by
    // at most 1e3 (about 1e-13 of Ms) and leaves a loop 1e-3 of the
    // reversible curve wide, anhysteretic to well under 1 % of Ms
    // (test_dsp_tape, test_reversibility_limit).
    static constexpr double kMaxReversibility = 0.999;

    // c outside [0, kMaxReversibility], or NaN, is clamped (NaN to the top);
    // params() returns what was kept.
    void set_params(const JilesAthertonParams& p) noexcept {
        p_ = p;
        if (!(p_.c <= kMaxReversibility)) p_.c = kMaxReversibility;
        if (p_.c < 0.0) p_.c = 0.0;
        inv_a_ = 1.0 / p.a;
        inv_k_ = 1.0 / p.k;
        inv_sub_h_ = 1.0 / (kSubstepH * p.a);
        sync();
    }
    const JilesAthertonParams& params() const noexcept { return p_; }
    // The processor's on_reset() calls it on the audio thread.
    void reset() noexcept SONORA_RT {
        m_ = 0.0;
        h_ = 0.0;
        sync();
    }
    double magnetisation() const noexcept { return m_; }

    // Susceptibility dM/dH at M = 0, the small-signal slope the
    // processor divides out: the irreversible part is zero there
    // (Man = Mirr = 0).
    double initial_susceptibility() const noexcept {
        const double s = p_.c * p_.ms / p_.a / 3.0;
        return s / (1.0 - p_.alpha * s);
    }

    // The field moves to `h` over one sample; returns M.
    double tick(double h) noexcept SONORA_RT;
    // tick() over a block, in place: x[i], the field, becomes M. Two
    // instances with the same parameters run together through
    // tape_run_pair(), to the same bits.
    void run(double* x, std::uint32_t n) noexcept SONORA_RT;

    // The ODE by classical RK4 over a given number of substeps, with the
    // exact Langevin function: the reference the tests hold tick() to, at
    // hundreds of substeps.
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
        sync();
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

    const LangevinTable& table() const noexcept { return table_; }

private:
    template <class V, class Bits>
    friend struct TapeSolve;
    // The algebraic form's state from (H, M). Reached from reset() and
    // tick_with(), both real-time.
    void sync() noexcept SONORA_RT {
        he_ = h_ + p_.alpha * m_;
        double l, dl;
        table_.eval(he_ * inv_a_, l, dl);
        man_ = p_.ms * l;
        dman_ = p_.ms * inv_a_ * dl;
        mirr_ = (m_ - p_.c * man_) / (1.0 - p_.c);
        // The reversible part alone: the next step's guess, corrected by
        // its Newton step.
        inv_den_ = 1.0 / (1.0 - p_.alpha * p_.c * dman_);
    }

    JilesAthertonParams p_;
    LangevinTable table_;
    double inv_a_ = 1.0, inv_k_ = 1.0, inv_sub_h_ = 1.0;
    std::uint32_t max_substeps_ = kMaxSubsteps;
    double m_ = 0.0, h_ = 0.0;
    // The algebraic form's state: the effective field, Man, its slope and
    // Mirr there, and 1 / (1 - alpha dM/dHe) at the end of the last step
    // (dHe/dH, the next step's guess).
    double he_ = 0.0, man_ = 0.0, dman_ = 0.0, mirr_ = 0.0, inv_den_ = 1.0;
};

// a.run(xa, n) and b.run(xb, n) at once, to the same bits, for two
// instances with the same parameters and substep cap (the processor's two
// channels): the operations of each run in the lanes of one register on
// x86-64 (src/arch/x86_64/tape_pair.cpp), side by side elsewhere.
void tape_run_pair(TapeHysteresis& a, TapeHysteresis& b, double* xa, double* xb, std::uint32_t n) noexcept SONORA_RT;

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
