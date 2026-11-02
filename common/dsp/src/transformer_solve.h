// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage's step (transformer.h), written once over a lane type V
// so one channel (Lane1, below) and two in lockstep
// (src/arch/<arch>/transformer_pair.cpp) run the same operations in the
// same order. With contraction off (common/dsp/CMakeLists.txt) each lane
// gives the bits the one-channel step gives; test_dsp_transformer holds
// that. The pattern is triode_solve.h's: the branches of the core's update
// are selects, so the two channels share every instruction, and a step the
// first evaluation does not settle leaves the lanes for the scalar loop.
//
// V provides: kLanes; V(double), a broadcast; V::load(p), lanes from
// p[0..kLanes); get(i); + - * /; neg(); abs(); sqrt(); V::select(mask, a,
// b); <, >, <= giving a Mask (ordered: false on a NaN); Mask &, lane(i).
#pragma once

#include <cmath>
#include <cstdint>

#include "sonora/dsp/transformer.h"

namespace sonora::dsp {

namespace transformer_detail {

// |u| is capped before u^4 (OutputStage::process_lanes).
constexpr double kUMax = 1e6;
// The Langevin table's span and density (OutputStage::lan_). Past kLanTop,
// coth is 1 to 1e-17 and L(x) = 1 - 1/x, L'(x) = 1/x^2 exactly in doubles.
constexpr double kLanTop = 20.0;
constexpr double kLanPerUnit = 32.0;
static_assert(kLanTop * kLanPerUnit == OutputStage::kLangevinSegments, "the table covers [0, kLanTop]");

}  // namespace transformer_detail

struct OutputStage::Lane1 {
    static constexpr int kLanes = 1;
    struct Mask {
        bool b;
        Mask operator&(Mask o) const noexcept { return {b && o.b}; }
        bool lane(int) const noexcept { return b; }
    };
    double x;
    Lane1() noexcept : x(0.0) {}
    explicit Lane1(double v) noexcept : x(v) {}
    static Lane1 load(const double* p) noexcept { return Lane1(p[0]); }
    double get(int) const noexcept { return x; }
    Lane1 operator+(Lane1 o) const noexcept { return Lane1(x + o.x); }
    Lane1 operator-(Lane1 o) const noexcept { return Lane1(x - o.x); }
    Lane1 operator*(Lane1 o) const noexcept { return Lane1(x * o.x); }
    Lane1 operator/(Lane1 o) const noexcept { return Lane1(x / o.x); }
    Lane1 neg() const noexcept { return Lane1(-x); }
    Lane1 abs() const noexcept { return Lane1(std::fabs(x)); }
    Lane1 sqrt() const noexcept { return Lane1(std::sqrt(x)); }
    Mask operator<(Lane1 o) const noexcept { return {x < o.x}; }
    Mask operator>(Lane1 o) const noexcept { return {x > o.x}; }
    Mask operator<=(Lane1 o) const noexcept { return {x <= o.x}; }
    static Lane1 select(Mask m, Lane1 a, Lane1 b) noexcept { return m.b ? a : b; }
};

template <class V>
struct OutputStage::EvalV {
    V psi, m, m_irr, dpsi, dm, dm_irr;  // values at i and their slopes in i
};

// The core at magnetising current i, from the state of the last step: the
// anhysteretic curve, the irreversible update and its guards (transformer.h).
template <class V>
OutputStage::EvalV<V> OutputStage::eval_lanes(const Core* const* s, V i) const noexcept SONORA_RT {
    using namespace transformer_detail;
    using M = typename V::Mask;
    constexpr int L = V::kLanes;
    double a_i[L], a_m[L], a_mirr[L];
    for (int l = 0; l < L; ++l) {
        a_i[l] = s[l]->i;
        a_m[l] = s[l]->m;
        a_mirr[l] = s[l]->m_irr;
    }
    const V si = V::load(a_i), sm = V::load(a_m), smirr = V::load(a_mirr);
    const V zero(0.0), one(1.0);
    const V dh = i - si;
    const V he = i + V(p_.alpha) * sm;
    const V x = he * V(inv_a_);
    const V ax = x.abs();
    // The Langevin function: from its table under kLanTop, cubic Hermite on
    // L and L' at the nodes, so the curve is C1, exact at every node, and
    // its slope is the interpolant's own (what the Newton solve converges
    // on). Its error is under h^4 max|L''''| / 384 = 1e-9 at h = 1/32,
    // which moves the output by about 1e-12 of full scale. It stands where
    // an exp and a division stood on the solve's chain of dependent
    // operations. Both forms are computed and one is selected; a NaN takes
    // the 1/x form, so it never reaches the index conversion.
    const M in_table = ax < V(kLanTop);
    const V p = ax * V(kLanPerUnit);
    double la[L], lb[L], da[L], db[L], kf[L];
    for (int l = 0; l < L; ++l) {
        const std::uint32_t k = in_table.lane(l) ? static_cast<std::uint32_t>(p.get(l)) : 0u;
        kf[l] = static_cast<double>(k);
        la[l] = lan_[k].l;
        da[l] = lan_[k].dl;
        lb[l] = lan_[k + 1].l;
        db[l] = lan_[k + 1].dl;
    }
    const V u = p - V::load(kf);
    const V h(1.0 / kLanPerUnit);
    const V al = V::load(la), bl = V::load(lb);
    const V ta = h * V::load(da), tb = h * V::load(db);
    const V dlv = al - bl;
    // p(u) = a.l + u ta + u^2 (3 (b.l - a.l) - 2 ta - tb) + u^3 (2 (a.l - b.l) + ta + tb)
    const V c2 = V(-3.0) * dlv - V(2.0) * ta - tb;
    const V c3 = V(2.0) * dlv + ta + tb;
    const V u2 = u * u;
    const V mag_t = al + u * ta + u2 * (c2 + u * c3);
    const V dl_t = (ta + u * (V(2.0) * c2 + V(3.0) * u * c3)) * V(kLanPerUnit);
    const V r = one / ax;
    const V mag = V::select(in_table, mag_t, one - r);
    const V dl = V::select(in_table, dl_t, r * r);
    const V lan = V::select(x < zero, mag.neg(), mag);

    const V man = V(p_.m_sat) * lan;
    const V dman = V(p_.m_sat * inv_a_) * dl;
    const V d = man - smirr;
    const V delta = V::select(dh > zero, one, V::select(dh < zero, V(-1.0), zero));
    // dMirr/dH = (Man - Mirr) / (delta k - alpha (Man - Mirr)) while Mirr
    // moves towards the curve. The denominator's floor is the guard against
    // the non-physical region (k < alpha |d|) in which the textbook form
    // runs backwards. One division for f and its slope.
    const M moving = (delta * d) > zero;
    const V ad = d.abs();
    const V floor_den(0.05 * p_.k);
    V den = V(p_.k) - V(p_.alpha) * ad;
    den = V::select(den < floor_den, floor_den, den);
    const V rr = one / den;
    const V f = ad * rr;
    const V m_try = smirr + dh * f;
    // A step must not carry Mirr past the curve it is chasing; on the
    // curve it moves with it.
    const M clamp = moving & ((delta * (m_try - man)) > zero);
    const V dm_try = f + dh.abs() * dman * V(p_.k) * (rr * rr);
    EvalV<V> e;
    e.m_irr = V::select(moving, V::select(clamp, man, m_try), smirr);
    e.dm_irr = V::select(moving, V::select(clamp, dman, dm_try), zero);
    e.m = e.m_irr + V(p_.c) * (man - e.m_irr);
    e.dm = V(1.0 - p_.c) * e.dm_irr + V(p_.c) * dman;
    e.psi = V(p_.l_core) * (i + e.m);
    e.dpsi = V(p_.l_core) * (one + e.dm);
    return e;
}

// One step for each lane's channel: solves F(i) = psi(i) - rhs + half i = 0
// for the new magnetising current and stores the new state.
//
// F is strictly increasing with slope >= half, so from F(i0) the root lies
// within |F(i0)| / half of i0. The state is the solved point of the last
// step, so F(s.i) needs no evaluation: s.psi is psi there. The first Newton
// step uses the reciprocal slope kept from that step (the core's slope moves
// little in one step). The Newton step left after the evaluation there is
// taken along the slope just evaluated, not evaluated again: where the core
// is smooth its error is second order in that step, and at a branch switch
// of the hysteresis at most the step itself, which di_tol_ bounds
// (set_solve_tolerance). On programme that is one evaluation per step
// (test_dsp_transformer, "solve"); refine() is for the steps the predictor
// misses by more (an edge, a reset), held to the same bound.
template <class V>
void OutputStage::step_lanes(Core* const* s, const double* rhs_in) noexcept SONORA_RT {
    constexpr int L = V::kLanes;
    double a_psi[L], a_i[L], a_inv[L];
    for (int l = 0; l < L; ++l) {
        a_psi[l] = s[l]->psi;
        a_i[l] = s[l]->i;
        a_inv[l] = s[l]->inv_slope;
    }
    const V half(half_);
    const V rhs = V::load(rhs_in);
    const V f0 = V::load(a_psi) - rhs + half * V::load(a_i);
    const V i = V::load(a_i) - f0 * V::load(a_inv);
    const EvalV<V> e = eval_lanes<V>(s, i);
    const V f = e.psi - rhs + half * i;
    const V inv = V(1.0) / (e.dpsi + half);
    const V di = (f * inv).neg();
    const typename V::Mask ok = di.abs() <= V(di_tol_);
    for (int l = 0; l < L; ++l) {
        Core& c = *s[l];
        double il = i.get(l), dil = di.get(l), invl = inv.get(l);
        EvalV<Lane1> el{Lane1(e.psi.get(l)), Lane1(e.m.get(l)),  Lane1(e.m_irr.get(l)),
                        Lane1(e.dpsi.get(l)), Lane1(e.dm.get(l)), Lane1(e.dm_irr.get(l))};
        std::uint32_t n = 1;
        if (!ok.lane(l)) {
            double fl = f.get(l);
            refine(c, rhs_in[l], il, el, fl, invl, dil, n);
        }
        if (n > max_iter_seen_) max_iter_seen_ = n;
        evals_ += n;
        ++steps_;
        c.psi = el.psi.x + el.dpsi.x * dil;
        c.m = el.m.x + el.dm.x * dil;
        c.m_irr = el.m_irr.x + el.dm_irr.x * dil;
        c.i = il + dil;
        c.inv_slope = invl;
    }
}

// `steps` core steps on every channel, V::kLanes channels at a time: one
// call per block, so the constants and the loop stay in registers.
template <class V>
void OutputStage::process_lanes(const double* const* in, double* const* out, std::uint32_t steps,
                                const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT {
    using namespace transformer_detail;
    constexpr int L = V::kLanes;
    const std::uint32_t ns = substeps_;
    for (std::uint32_t t = 0, at = 0; t < steps; ++t, at += ns) {
        if (sag != nullptr) set_sag(sag[t / steps_per_sag]);
        const double rail = rail_;
        const V railv(rail);
        const V u_per_x(ckt_.volts_per_unit / rail);
        double drawn = 0.0;
        for (std::uint32_t c0 = 0; c0 + L <= channels_; c0 += L) {
            Core* s[L];
            for (int l = 0; l < L; ++l) s[l] = &ch_[c0 + l];
            V vs[kMaxSubsteps];
            for (std::uint32_t j = 0; j < ns; ++j) {
                double xs[L];
                for (int l = 0; l < L; ++l) {
                    const double x = in[c0 + l][at + j];
                    xs[l] = std::isfinite(x) ? x : 0.0;
                }
                // The driver, rail-limited: Vs = rail u / (1 + u^4)^(1/4). |u| is
                // capped first. Past 1e6 the curve is flat at the rail to 1e-6
                // relative; uncapped, u^4 overflowed from |x| of about 1.3e77,
                // and vs collapsed to rail u / inf = 0 (the stage went silent
                // under a huge input instead of clipping); above about 7.2e305
                // x * vpu itself overflowed and vs = inf / inf was NaN.
                V u = V::load(xs) * u_per_x;
                u = V::select(u < V(kUMax), u, V(kUMax));
                u = V::select(u > V(-kUMax), u, V(-kUMax));
                const V u2 = u * u;
                vs[j] = railv * u / (V(1.0) + u2 * u2).sqrt().sqrt();
            }
            // The node equation's flux over the step: the driver's share by the
            // trapezoid on every sample, the magnetising current's exactly for
            // the current taken linear across the step (which is how it is read
            // back below). With one substep this is the plain trapezoid.
            double a_last[L], a_psi[L], a_i[L];
            for (int l = 0; l < L; ++l) {
                a_last[l] = s[l]->vs_last;
                a_psi[l] = s[l]->psi;
                a_i[l] = s[l]->i;
            }
            V sum = V(0.5) * (V::load(a_last) + vs[ns - 1]);
            for (std::uint32_t j = 0; j + 1 < ns; ++j) sum = sum + vs[j];
            const V i0 = V::load(a_i);
            const V rhs = V::load(a_psi) + V(t_ * g_) * sum - V(half_) * i0;
            double a_rhs[L];
            for (int l = 0; l < L; ++l) a_rhs[l] = rhs.get(l);
            step_lanes<V>(s, a_rhs);
            double a_i1[L];
            for (int l = 0; l < L; ++l) {
                s[l]->vs_last = vs[ns - 1].get(l);
                a_i1[l] = s[l]->i;
            }
            const V di = (V::load(a_i1) - i0) * V(inv_substeps_);
            V load(0.0);
            for (std::uint32_t j = 0; j < ns; ++j) {
                const V v = V(g_) * vs[j] - V(rp_) * (i0 + V(static_cast<double>(j + 1)) * di);
                // The primary's current, from the driver: push-pull draws it
                // from the rail on both half cycles.
                load = load + (vs[j] - v).abs();
                const V y = v * V(inv_out_);
                for (int l = 0; l < L; ++l) {
                    const double yl = y.get(l);
                    out[c0 + l][at + j] = std::isfinite(yl) ? yl : 0.0;
                }
            }
            for (int l = 0; l < L; ++l) drawn += load.get(l) * inv_rs_ * inv_substeps_;
        }
        if (rsup_ <= 1e-9) {
            vres_ = v_idle_;
        } else {
            // Backward Euler on the reservoir (set_sag): stable at any step,
            // however light the source resistance.
            vres_ = (vres_ + sup_a_ * (sup_open_ - ckt_.i_quiescent - drawn)) * sup_inv_den_;
        }
        rail_ = vres_ - ckt_.v_knee;
        if (rail_ < 10.0) rail_ = 10.0;
    }
}

}  // namespace sonora::dsp
