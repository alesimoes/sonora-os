// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape hysteresis' per-sample integrator (tape.h, TapeHysteresis),
// written once over a lane type V, as triode_solve.h is: one instance
// (TapeHysteresis::tick(), V of one double, in tape.cpp) and the
// processor's two channels in lockstep (tape_run_pair(), V of two
// doubles, in src/arch/<arch>/tape_pair.cpp) run the same operations in
// the same order, so a channel's output does not depend on whether it ran
// alone or beside another (test_dsp_tape checks it to the bit). Every
// branch of the scalar form is a select here: both sides are computed
// where any lane needs them, and each lane keeps its own. Internal to
// common/dsp.
//
// V is the shared two-lane type of src/arch/<arch>/f64x2.h (F64x2), or
// tape_detail::V1 for one lane, through the subset of its interface that
// tape_detail::LaneTypeCheck asserts: V::kLanes; V(double) broadcast;
// V::load(const double*), the lanes from an array; get(lane); + - * /;
// neg; abs; comparisons < > <= == returning V::Mask; select(mask, a, b);
// Mask with & | ~ and any(); trunc_index(int*); gather2(rows, lo, hi).
// Bits supplies the one operation F64x2 does not have:
// Bits::exp2_magic(t), 2^k for t = k + kMagic (the rounding sum below), by
// moving k's bits into the exponent: the same bits whatever the lane type.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#include "sonora/dsp/tape.h"

namespace sonora::dsp {

namespace tape_detail {

// x + 1.5 * 2^52 - 1.5 * 2^52 rounds x to the nearest integer in IEEE
// double arithmetic (|x| < 2^51), and leaves that integer in the sum's low
// mantissa bits.
constexpr double kMagic = 6755399441055744.0;
constexpr double kInvLn2 = 1.4426950408889634;
// ln 2 in two parts, the first with trailing zero bits so k * kLn2Hi is
// exact (W. J. Cody and W. Waite, "Software Manual for the Elementary
// Functions", 1980, ch. 6).
constexpr double kLn2Hi = 6.93147180369123816490e-01;
constexpr double kLn2Lo = 1.90821492927058770002e-10;
// Beyond it e^-tau is under 5e-18: 1 - e^-tau is 1 in double.
constexpr double kTauMax = 40.0;
// The series of the w_j below holds to 1e-10 relative under it.
constexpr double kTauSeries = 0.01;

// 2^k for t = k + kMagic: k's bits moved into the exponent. Every Bits
// policy computes this, lane by lane or in one register.
inline double exp2_bits(double t) noexcept SONORA_RT {
    std::uint64_t u;
    std::memcpy(&u, &t, sizeof u);
    u = (u + 1023u) << 52;
    double r;
    std::memcpy(&r, &u, sizeof r);
    return r;
}

// One lane: the scalar tick(), with F64x2's interface.
struct V1 {
    static constexpr int kLanes = 1;
    struct Mask {
        bool b;
        Mask operator&(Mask o) const noexcept { return {b && o.b}; }
        Mask operator|(Mask o) const noexcept { return {b || o.b}; }
        Mask operator~() const noexcept { return {!b}; }
        bool any() const noexcept { return b; }
    };
    double x;
    V1() noexcept : x(0.0) {}
    explicit V1(double v) noexcept : x(v) {}
    static V1 load(const double* p) noexcept { return V1(p[0]); }
    double get(int) const noexcept { return x; }
    static void gather2(const double* const* p, V1& lo, V1& hi) noexcept {
        lo = V1(p[0][0]);
        hi = V1(p[0][1]);
    }
    V1 trunc_index(int* idx) const noexcept {
        idx[0] = static_cast<int>(x);
        return V1(static_cast<double>(idx[0]));
    }
    V1 operator+(V1 o) const noexcept { return V1(x + o.x); }
    V1 operator-(V1 o) const noexcept { return V1(x - o.x); }
    V1 operator*(V1 o) const noexcept { return V1(x * o.x); }
    V1 operator/(V1 o) const noexcept { return V1(x / o.x); }
    V1 neg() const noexcept { return V1(-x); }
    V1 abs() const noexcept { return V1(std::fabs(x)); }
    Mask operator<(V1 o) const noexcept { return {x < o.x}; }
    Mask operator>(V1 o) const noexcept { return {x > o.x}; }
    Mask operator<=(V1 o) const noexcept { return {x <= o.x}; }
    Mask operator==(V1 o) const noexcept { return {x == o.x}; }
    static V1 select(Mask m, V1 a, V1 b) noexcept { return m.b ? a : b; }
};
struct V1Bits {
    static V1 exp2_magic(V1 t) noexcept { return V1(exp2_bits(t.x)); }
};

// The lane type's requirements, checked where TapeSolve is instantiated,
// so a type that lacks one fails with the requirement named rather than
// deep inside the solve (C++17 detection through std::void_t).
template <class V>
using Mask_t = typename V::Mask;

template <class V, class = void>
struct HasArithmetic : std::false_type {};
template <class V>
struct HasArithmetic<V, std::void_t<decltype(std::declval<V>() + std::declval<V>()),
                                    decltype(std::declval<V>() - std::declval<V>()),
                                    decltype(std::declval<V>() * std::declval<V>()),
                                    decltype(std::declval<V>() / std::declval<V>()),
                                    decltype(std::declval<const V&>().neg()), decltype(std::declval<const V&>().abs())>>
    : std::integral_constant<bool, std::is_same<decltype(std::declval<V>() + std::declval<V>()), V>::value &&
                                       std::is_same<decltype(std::declval<V>() - std::declval<V>()), V>::value &&
                                       std::is_same<decltype(std::declval<V>() * std::declval<V>()), V>::value &&
                                       std::is_same<decltype(std::declval<V>() / std::declval<V>()), V>::value &&
                                       std::is_same<decltype(std::declval<const V&>().neg()), V>::value &&
                                       std::is_same<decltype(std::declval<const V&>().abs()), V>::value> {};

template <class V, class = void>
struct HasCompare : std::false_type {};
template <class V>
struct HasCompare<V, std::void_t<Mask_t<V>, decltype(std::declval<V>() < std::declval<V>()),
                                 decltype(std::declval<V>() > std::declval<V>()),
                                 decltype(std::declval<V>() <= std::declval<V>()),
                                 decltype(std::declval<V>() == std::declval<V>())>>
    : std::integral_constant<bool, std::is_same<decltype(std::declval<V>() < std::declval<V>()), Mask_t<V>>::value &&
                                       std::is_same<decltype(std::declval<V>() > std::declval<V>()), Mask_t<V>>::value &&
                                       std::is_same<decltype(std::declval<V>() <= std::declval<V>()), Mask_t<V>>::value &&
                                       std::is_same<decltype(std::declval<V>() == std::declval<V>()), Mask_t<V>>::value> {};

template <class V, class = void>
struct HasMaskSelect : std::false_type {};
template <class V>
struct HasMaskSelect<V, std::void_t<decltype(std::declval<Mask_t<V>>() & std::declval<Mask_t<V>>()),
                                    decltype(std::declval<Mask_t<V>>() | std::declval<Mask_t<V>>()),
                                    decltype(~std::declval<Mask_t<V>>()), decltype(std::declval<const Mask_t<V>&>().any()),
                                    decltype(V::select(std::declval<Mask_t<V>>(), std::declval<V>(), std::declval<V>()))>>
    : std::integral_constant<
          bool, std::is_same<decltype(std::declval<Mask_t<V>>() & std::declval<Mask_t<V>>()), Mask_t<V>>::value &&
                    std::is_same<decltype(std::declval<Mask_t<V>>() | std::declval<Mask_t<V>>()), Mask_t<V>>::value &&
                    std::is_same<decltype(~std::declval<Mask_t<V>>()), Mask_t<V>>::value &&
                    std::is_same<decltype(std::declval<const Mask_t<V>&>().any()), bool>::value &&
                    std::is_same<decltype(V::select(std::declval<Mask_t<V>>(), std::declval<V>(), std::declval<V>())),
                                 V>::value> {};

template <class V, class = void>
struct HasLanes : std::false_type {};
template <class V>
struct HasLanes<V, std::void_t<decltype(V::kLanes), decltype(V(1.0)), decltype(V::load(std::declval<const double*>())),
                               decltype(std::declval<const V&>().get(0)),
                               decltype(std::declval<const V&>().trunc_index(std::declval<int*>())),
                               decltype(V::gather2(std::declval<const double* const*>(), std::declval<V&>(),
                                                   std::declval<V&>()))>>
    : std::integral_constant<bool, std::is_same<decltype(V::load(std::declval<const double*>())), V>::value &&
                                       std::is_same<decltype(std::declval<const V&>().get(0)), double>::value &&
                                       std::is_same<decltype(std::declval<const V&>().trunc_index(std::declval<int*>())),
                                                    V>::value &&
                                       (V::kLanes >= 1)> {};

template <class V, class Bits, class = void>
struct HasExp2 : std::false_type {};
template <class V, class Bits>
struct HasExp2<V, Bits, std::void_t<decltype(Bits::exp2_magic(std::declval<V>()))>>
    : std::is_same<decltype(Bits::exp2_magic(std::declval<V>())), V> {};

template <class V, class Bits>
struct LaneTypeCheck {
    static_assert(HasArithmetic<V>::value, "TapeSolve's V: + - * / neg() abs(), each returning V");
    static_assert(HasCompare<V>::value, "TapeSolve's V: < > <= == returning V::Mask");
    static_assert(HasMaskSelect<V>::value, "TapeSolve's V::Mask: & | ~ any(); V::select(Mask, V, V) returning V");
    static_assert(HasLanes<V>::value,
                  "TapeSolve's V: kLanes >= 1, V(double), V::load(const double*), get(int) returning double, "
                  "trunc_index(int*) returning V, V::gather2(const double* const*, V&, V&)");
    static_assert(HasExp2<V, Bits>::value, "TapeSolve's Bits: static V exp2_magic(V)");
    static constexpr bool value = true;
};

}  // namespace tape_detail

template <class V, class Bits>
struct TapeSolve {
    static_assert(tape_detail::LaneTypeCheck<V, Bits>::value, "TapeSolve: lane type");
    static constexpr int L = V::kLanes;
    using Mask = typename V::Mask;

    static V sel(Mask m, V a, V b) noexcept { return V::select(m, a, b); }
    static V lanes(TapeHysteresis* const* st, double TapeHysteresis::*field) noexcept {
        double v[L];
        for (int i = 0; i < L; ++i) v[i] = st[i]->*field;
        return V::load(v);
    }
    static void put(TapeHysteresis* const* st, double TapeHysteresis::*field, V x) noexcept {
        for (int i = 0; i < L; ++i) st[i]->*field = x.get(i);
    }

    // e^-tau for tau in [0, kTauMax]: Cody-Waite reduction to
    // r in [-ln2/2, ln2/2], then the Taylor polynomial to r^8, whose
    // remainder is under (ln2/2)^9 / 9! = 2.1e-10 relative (2.7e-10 with
    // the rounding, measured over [0, 40]; test_dsp_tape holds it to 5e-10
    // against std::exp): far below the integrator's own error. Written out rather than std::exp so two lanes compute it at
    // once, with the same bits in both.
    static V exp_neg(V tau) noexcept {
        using namespace tape_detail;
        const V x = tau.neg();
        const V t = x * V(kInvLn2) + V(kMagic);
        const V k = t - V(kMagic);
        const V r = (x - k * V(kLn2Hi)) - k * V(kLn2Lo);
        const V r2 = r * r, r4 = r2 * r2, r8 = r4 * r4;
        const V a01 = V(1.0) + r;
        const V a23 = V(1.0 / 2.0) + r * V(1.0 / 6.0);
        const V a45 = V(1.0 / 24.0) + r * V(1.0 / 120.0);
        const V a67 = V(1.0 / 720.0) + r * V(1.0 / 5040.0);
        const V p = ((a01 + r2 * a23) + r4 * (a45 + r2 * a67)) + r8 * V(1.0 / 40320.0);
        return p * Bits::exp2_magic(t);
    }

    // L(x) and L'(x) by the table (LangevinTable::eval, lane by lane).
    static void langevin(const LangevinTable& tab, V x, V& l, V& dl) noexcept {
        const V ax = x.abs();
        const Mask far = ~(ax < V(LangevinTable::kMax));
        const V u = sel(far, V(0.0), ax) * V(static_cast<double>(LangevinTable::kPerUnit));
        int cell[L];
        const V t = u - u.trunc_index(cell);
        // A cell is four adjacent doubles: two gathers of a pair each.
        const double* rows[L];
        const double* rows_hi[L];
        for (int i = 0; i < L; ++i) {
            rows[i] = tab.cell(cell[i]);
            rows_hi[i] = rows[i] + 2;
        }
        V k0, k1, k2, k3;
        V::gather2(rows, k0, k1);
        V::gather2(rows_hi, k2, k3);
        const V t2 = t * t;
        V v = (k0 + t * k1) + t2 * (k2 + t * k3);
        V d = ((k1 + V(2.0) * t * k2) + V(3.0) * t2 * k3) * V(static_cast<double>(LangevinTable::kPerUnit));
        if (far.any()) {
            const V r = V(1.0) / sel(far, ax, V(1.0));
            v = sel(far, V(1.0) - r, v);
            d = sel(far, r * r, d);
        }
        l = sel(x < V(0.0), v.neg(), v);
        dl = d;
    }

    struct State {
        V he, man, dman, mirr, m, inv_den;
    };

    // One substep, in the lanes `on`: the field moves by dh to h1. The
    // header's comment on TapeHysteresis has the method.
    static void advance(const TapeHysteresis& p0, const LangevinTable& tab, State& s, V h1, V dh, V delta,
                        Mask on) noexcept {
        using namespace tape_detail;
        const JilesAthertonParams& p = p0.p_;
        const V ms(p.ms), c(p.c), c1(1.0 - p.c), alpha(p.alpha), inv_a(p0.inv_a_), inv_k(p0.inv_k_);
        const V zero(0.0), one(1.0);
        // The guess: He moves by dH / (1 - alpha dM/dHe) with the last
        // step's susceptibility; never backwards.
        V he1 = s.he + dh * s.inv_den;
        he1 = sel(delta * (he1 - s.he) < zero, s.he, he1);
        V l, dl, lm, dlm;
        langevin(tab, he1 * inv_a, l, dl);
        langevin(tab, (s.he + he1) * V(0.5) * inv_a, lm, dlm);
        V g1 = ms * l;
        const V dg1 = ms * inv_a * dl;
        const V gm = ms * lm;

        // Mirr over the distance d the effective field travels: Man along
        // it is the quartic through Man and its slope at the two ends and
        // Man halfway, in sigma = s / d:
        //   g0 + b sigma + p2 sigma^2 + p3 sigma^3 + p4 sigma^4.
        const V y0 = s.mirr, g0 = s.man;
        const V d = delta * (he1 - s.he);
        V b = delta * s.dman * d;
        const V b1 = delta * dg1 * d;
        const V e1 = g1 - g0 - b, e2 = b1 - b, e3 = gm - g0 - V(0.5) * b;
        V p2 = (e2 - V(5.0) * e1) + V(16.0) * e3;
        V p3 = (V(14.0) * e1 - V(3.0) * e2) - V(32.0) * e3;
        V p4 = (V(2.0) * e2 - V(8.0) * e1) + V(16.0) * e3;
        V start = g0, dr = d;
        // Mirr leads Man (after a reversal): it holds until Man reaches
        // it, then relaxes over the rest of the step.
        const Mask lead = delta * (g0 - y0) < zero;
        const Mask hold = lead & (delta * (g1 - y0) <= zero);
        const Mask cross = lead & ~hold;
        if (cross.any()) {
            V sg = (y0 - g0) / sel(cross, g1 - g0, one);
            for (int i = 0; i < 2; ++i) {
                const V fp = b + sg * (V(2.0) * p2 + sg * (V(3.0) * p3 + sg * (V(4.0) * p4)));
                const Mask flat = fp == zero;
                const V f = g0 + sg * (b + sg * (p2 + sg * (p3 + sg * p4))) - y0;
                sg = sel(flat, sg, sg - f / sel(flat, one, fp));
                sg = sel(sg < zero, zero, sel(sg > one, one, sg));
            }
            // The same quartic over [sg, 1], rescaled to [0, 1]: its
            // Taylor coefficients at sg times powers of the remainder.
            const V r = one - sg;
            const V r2 = r * r, r3 = r2 * r;
            const V bc = r * (b + sg * (V(2.0) * p2 + sg * (V(3.0) * p3 + sg * (V(4.0) * p4))));
            const V c2 = r2 * (p2 + sg * (V(3.0) * p3 + sg * (V(6.0) * p4)));
            const V c3 = r3 * (p3 + sg * (V(4.0) * p4));
            const V c4 = r3 * r * p4;
            b = sel(cross, bc, b);
            p2 = sel(cross, c2, p2);
            p3 = sel(cross, c3, p3);
            p4 = sel(cross, c4, p4);
            start = sel(cross, y0, start);
            dr = sel(cross, d * r, dr);
        }
        // w_j, the integral over [0, 1] of tau exp(-tau (1 - sigma)) sigma^j:
        // Mirr's response to each term of the cubic. A small tau (and a
        // step that does not move, tau = 0) takes the series, where the
        // closed forms cancel; the closed forms are the recurrence
        // w_j = 1 - j w_(j-1) / tau unrolled, so its terms run side by side.
        const V tau = dr * inv_k;
        const Mask small = tau < V(kTauSeries);
        V w0, w1, w2, w3, w4;
        if ((~small).any()) {
            V tc = sel(small, one, tau);
            tc = sel(tc > V(kTauMax), V(kTauMax), tc);
            const V it = one / tc, it2 = it * it;
            w0 = one - exp_neg(tc);
            const V w0i = w0 * it;
            const V w0i2 = w0i * it, w0i3 = w0i2 * it, w0i4 = w0i3 * it;
            const V it3 = it2 * it;
            w1 = one - w0i;
            w2 = (one - V(2.0) * it) + V(2.0) * w0i2;
            w3 = ((one - V(3.0) * it) + V(6.0) * it2) - V(6.0) * w0i3;
            w4 = (((one - V(4.0) * it) + V(12.0) * it2) - V(24.0) * it3) + V(24.0) * w0i4;
        }
        if (small.any()) {
            // w_j = j! sum_n (-1)^n tau^(n+1) / (n+j+1)!, four terms.
            const V t2 = tau * tau, t3 = t2 * tau, t4 = t3 * tau;
            const V s0 = ((tau - t2 * V(1.0 / 2.0)) + t3 * V(1.0 / 6.0)) - t4 * V(1.0 / 24.0);
            const V s1 = ((tau * V(1.0 / 2.0) - t2 * V(1.0 / 6.0)) + t3 * V(1.0 / 24.0)) - t4 * V(1.0 / 120.0);
            const V s2 = ((tau * V(1.0 / 3.0) - t2 * V(1.0 / 12.0)) + t3 * V(1.0 / 60.0)) - t4 * V(1.0 / 360.0);
            const V s3 = ((tau * V(1.0 / 4.0) - t2 * V(1.0 / 20.0)) + t3 * V(1.0 / 120.0)) - t4 * V(1.0 / 840.0);
            const V s4 = ((tau * V(1.0 / 5.0) - t2 * V(1.0 / 30.0)) + t3 * V(1.0 / 210.0)) - t4 * V(1.0 / 1680.0);
            w0 = sel(small, s0, w0);
            w1 = sel(small, s1, w1);
            w2 = sel(small, s2, w2);
            w3 = sel(small, s3, w3);
            w4 = sel(small, s4, w4);
        }
        // A tree, not a chain: the five products are independent.
        V y = (y0 + (start - y0) * w0) + ((b * w1 + p2 * w2) + (p3 * w3 + p4 * w4));
        y = sel(hold, y0, y);
        y = clamp(y, ms);
        const V rise = delta * (g1 - y);
        const V dy = sel(rise > zero, rise * inv_k, zero);

        V m1 = c * g1 + c1 * y;
        const V chi1 = c * dg1 + c1 * dy;
        V fd = one - alpha * chi1;
        fd = sel(fd < V(0.25), V(0.25), fd);
        const V inv_den = one / fd;
        // One Newton step on F(He1) = H1 + alpha M(He1) - He1, applied to
        // the state to first order rather than by a second evaluation:
        // the guess is off by alpha times the change in susceptibility
        // over the step, and the linearisation's error is its square.
        const V step = (h1 + alpha * m1 - he1) * inv_den;
        he1 = he1 + step;
        g1 = g1 + dg1 * step;
        y = y + dy * step;
        m1 = m1 + chi1 * step;
        s.he = sel(on, he1, s.he);
        s.man = sel(on, g1, s.man);
        s.dman = sel(on, dg1, s.dman);
        s.mirr = sel(on, clamp(y, ms), s.mirr);
        s.m = sel(on, clamp(m1, ms), s.m);
        s.inv_den = sel(on, inv_den, s.inv_den);
    }

    static V clamp(V x, V lim) noexcept { return sel(x > lim, lim, sel(x < lim.neg(), lim.neg(), x)); }

    // One sample: the field moves from h0 to h. Returns the field the
    // state now stands at (h, or h0 in a lane the guard leaves).
    static V sample(const TapeHysteresis& p0, const LangevinTable& tab, State& s, V h0, V h) noexcept {
        using namespace tape_detail;
        const V dh = h - h0;
        const V adh = dh.abs();
        // A non-finite field (or none) leaves a lane as it was.
        const Mask on = (adh > V(0.0)) & (adh < V(1e300));
        const V delta = sel(dh > V(0.0), V(1.0), V(-1.0));
        if (p0.max_substeps_ == 1) {
            advance(p0, tab, s, h, dh, delta, on);
            return sel(on, h, h0);
        }
        // ceil(|dh| / sub_h), at least 1, at most the cap; 1 in a lane
        // that is off.
        const V cap(static_cast<double>(p0.max_substeps_));
        V nf = adh * V(p0.inv_sub_h_);
        nf = sel(nf > cap, cap, nf);
        V count = (nf + V(kMagic)) - V(kMagic);
        count = sel(count < nf, count + V(1.0), count);
        count = sel(on & (count > V(1.0)), count, V(1.0));
        double most = count.get(0);
        for (int i = 1; i < L; ++i) most = count.get(i) > most ? count.get(i) : most;
        const V step = dh / count;
        V from = h0;
        for (double j = 1.0; j <= most; j += 1.0) {
            const V jv(j);
            const Mask live = on & (jv <= count);
            const V to = sel(jv == count, h, h0 + step * jv);
            advance(p0, tab, s, to, to - from, delta, live);
            from = sel(live, to, from);
        }
        return sel(on, h, h0);
    }

    // x[lane][i], the field, becomes M, for n samples: L instances with
    // the same parameters and cap (st[0]'s), the state held in registers
    // across the block.
    static void run(TapeHysteresis* const* st, double* const* x, std::uint32_t n) noexcept {
        const TapeHysteresis& p0 = *st[0];
        const LangevinTable& tab = p0.table_;
        State s;
        s.he = lanes(st, &TapeHysteresis::he_);
        s.man = lanes(st, &TapeHysteresis::man_);
        s.dman = lanes(st, &TapeHysteresis::dman_);
        s.mirr = lanes(st, &TapeHysteresis::mirr_);
        s.m = lanes(st, &TapeHysteresis::m_);
        s.inv_den = lanes(st, &TapeHysteresis::inv_den_);
        V h0 = lanes(st, &TapeHysteresis::h_);
        for (std::uint32_t i = 0; i < n; ++i) {
            double hv[L];
            for (int c = 0; c < L; ++c) hv[c] = x[c][i];
            h0 = sample(p0, tab, s, h0, V::load(hv));
            for (int c = 0; c < L; ++c) x[c][i] = s.m.get(c);
        }
        put(st, &TapeHysteresis::he_, s.he);
        put(st, &TapeHysteresis::man_, s.man);
        put(st, &TapeHysteresis::dman_, s.dman);
        put(st, &TapeHysteresis::mirr_, s.mirr);
        put(st, &TapeHysteresis::m_, s.m);
        put(st, &TapeHysteresis::inv_den_, s.inv_den);
        put(st, &TapeHysteresis::h_, h0);
    }
};

}  // namespace sonora::dsp
