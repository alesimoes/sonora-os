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
// b); <, >, <= giving a Mask (ordered: false on a NaN); Mask &, lane(i),
// all(); V::gather2(p, lo, hi), lane l from p[l][0] and p[l][1];
// trunc_index(idx), the lanes truncated to int.
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
        bool all() const noexcept { return b; }
    };
    double x;
    Lane1() noexcept : x(0.0) {}
    explicit Lane1(double v) noexcept : x(v) {}
    static Lane1 load(const double* p) noexcept { return Lane1(p[0]); }
    static void gather2(const double* const* p, Lane1& lo, Lane1& hi) noexcept {
        lo = Lane1(p[0][0]);
        hi = Lane1(p[0][1]);
    }
    Lane1 trunc_index(int* idx) const noexcept {
        idx[0] = static_cast<int>(x);
        return Lane1(static_cast<double>(idx[0]));
    }
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

template <class V>
struct OutputStage::StateV {
    V psi, i, m, m_irr, vs_last, inv_slope;  // Core's fields, one channel per lane
};

template <class V>
struct OutputStage::CoefV {
    V alpha, inv_a, m_sat, dman, k, floor_den, c, one_minus_c, l_core;
    const double (*lan)[2];
};

template <class V>
inline OutputStage::CoefV<V> OutputStage::coef() const noexcept SONORA_RT {
    return {V(p_.alpha),     V(inv_a_), V(p_.m_sat),     V(p_.m_sat * inv_a_), V(p_.k),
            V(0.05 * p_.k), V(p_.c),   V(1.0 - p_.c),   V(p_.l_core),         lan_};
}

// The core at magnetising current i, from the state of the last step (si,
// sm, smirr): the anhysteretic curve, the irreversible update and its
// guards (transformer.h).
template <class V>
inline OutputStage::EvalV<V> OutputStage::eval_lanes(const CoefV<V>& k, V si, V sm, V smirr, V i) noexcept SONORA_RT {
    using namespace transformer_detail;
    using M = typename V::Mask;
    constexpr int L = V::kLanes;
    const V zero(0.0), one(1.0);
    const V dh = i - si;
    const V he = i + k.alpha * sm;
    const V x = he * k.inv_a;
    const V ax = x.abs();
    // The Langevin function: from its table under kLanTop, cubic Hermite on
    // L and L' at the nodes, so the curve is C1, exact at every node, and
    // its slope is the interpolant's own (what the Newton solve converges
    // on). Its error is under h^4 max|L''''| / 384 = 1e-9 at h = 1/32,
    // which moves the output by about 1e-12 of full scale. It stands where
    // an exp and a division stood on the solve's chain of dependent
    // operations. Both forms are computed and one is selected; a NaN takes
    // the 1/x form, so it never reaches the index conversion.
    // The node index in vector form, and each node's (L, L') as one load
    // per lane: on the chain, a round trip through general registers and
    // the stack cost more than the interpolation.
    const M in_table = ax < V(kLanTop);
    const V p = ax * V(kLanPerUnit);
    int n[L];
    const V kf = V::select(in_table, p, zero).trunc_index(n);
    const double* na[L];
    const double* nb[L];
    for (int l = 0; l < L; ++l) {
        na[l] = k.lan[n[l]];
        nb[l] = k.lan[n[l] + 1];
    }
    V al, da, bl, db;
    V::gather2(na, al, da);
    V::gather2(nb, bl, db);
    const V u = p - kf;
    const V h(1.0 / kLanPerUnit);
    const V ta = h * da, tb = h * db;
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

    const V man = k.m_sat * lan;
    const V dman = k.dman * dl;
    const V d = man - smirr;
    const V delta = V::select(dh > zero, one, V::select(dh < zero, V(-1.0), zero));
    // dMirr/dH = (Man - Mirr) / (delta k - alpha (Man - Mirr)) while Mirr
    // moves towards the curve. The denominator's floor is the guard against
    // the non-physical region (k < alpha |d|) in which the textbook form
    // runs backwards. One division for f and its slope.
    const M moving = (delta * d) > zero;
    const V ad = d.abs();
    const V floor_den = k.floor_den;
    V den = k.k - k.alpha * ad;
    den = V::select(den < floor_den, floor_den, den);
    const V rr = one / den;
    const V f = ad * rr;
    const V m_try = smirr + dh * f;
    // A step must not carry Mirr past the curve it is chasing; on the
    // curve it moves with it.
    const M clamp = moving & ((delta * (m_try - man)) > zero);
    const V dm_try = f + dh.abs() * dman * k.k * (rr * rr);
    EvalV<V> e;
    e.m_irr = V::select(moving, V::select(clamp, man, m_try), smirr);
    e.dm_irr = V::select(moving, V::select(clamp, dman, dm_try), zero);
    e.m = e.m_irr + k.c * (man - e.m_irr);
    e.dm = k.one_minus_c * e.dm_irr + k.c * dman;
    e.psi = k.l_core * (i + e.m);
    e.dpsi = k.l_core * (one + e.dm);
    return e;
}

// `steps` core steps on every channel, V::kLanes channels at a time: one
// call per block, the same to the bit as `steps` calls of one step
// (test_dsp_transformer). The step is a chain of dependent operations, so
// what it costs is the chain's length: the cores' state stays in lane
// layout across the block (stored once at its end), the constants are
// broadcast locals the output stores cannot alias, the table is read one
// node per load, and the driver's clip for the next step runs beside this
// step's solve (rail_drv_). Against the per-step form with the state in
// Core: 0.57 to 0.69 of the time per block (brain/research/native-tube-n2.md,
// "Output stage at class B", the rerun).
//
// The solve, for each lane's channel: F(i) = psi(i) - rhs + half i = 0 for
// the new magnetising current. F is strictly increasing with slope >= half,
// so from F(i0) the root lies within |F(i0)| / half of i0. The state is the
// solved point of the last step, so F(s.i) needs no evaluation: s.psi is psi
// there. The first Newton step uses the reciprocal slope kept from that
// step (the core's slope moves little in one step). The Newton step left
// after the evaluation there is taken along the slope just evaluated, not
// evaluated again: where the core is smooth its error is second order in
// that step, and at a branch switch of the hysteresis at most the step
// itself, which di_tol_ bounds (set_solve_tolerance). On programme that is
// one evaluation per step (test_dsp_transformer, "solve"); refine() is for
// the steps the predictor misses by more (an edge, a reset), held to the
// same bound, one lane at a time.
template <class V>
void OutputStage::process_lanes(const double* const* in, double* const* out, std::uint32_t steps,
                                const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT {
    using namespace transformer_detail;
    using M = typename V::Mask;
    constexpr int L = V::kLanes;
    constexpr std::uint32_t G = kMaxChannels / L;
    const std::uint32_t groups = channels_ / L;
    const std::uint32_t ns = substeps_;
    const CoefV<V> k = coef<V>();
    const V tg(t_ * g_), g(g_), rp(rp_), inv_out(inv_out_), inv_sub(inv_substeps_), di_tol(di_tol_);
    const double inv_rs = inv_rs_, inv_sub_s = inv_substeps_, vpu = ckt_.volts_per_unit;
    const double i_q = ckt_.i_quiescent, v_knee = ckt_.v_knee, v_idle = v_idle_;
    const V zero(0.0), half(half_);
    // |x| <= DBL_MAX is std::isfinite: the compare is ordered, so a NaN
    // fails it too.
    const V finite_max(1.7976931348623157e308);
    StateV<V> st[G];
    for (std::uint32_t gi = 0; gi < groups; ++gi) {
        double a[6][L];
        for (int l = 0; l < L; ++l) {
            const Core& c = ch_[gi * L + l];
            a[0][l] = c.psi;
            a[1][l] = c.i;
            a[2][l] = c.m;
            a[3][l] = c.m_irr;
            a[4][l] = c.vs_last;
            a[5][l] = c.inv_slope;
        }
        st[gi] = {V::load(a[0]), V::load(a[1]), V::load(a[2]), V::load(a[3]), V::load(a[4]), V::load(a[5])};
    }
    std::uint32_t max_n = max_iter_seen_;
    std::uint64_t evals = evals_, nsteps = steps_;
    double vres = vres_, rail = rail_;
    // The driver's samples for one step on every group, clipped to rail r.
    // Step t's are made during step t - 1, before its solve (the driver's
    // rail lags by a step, rail_drv_), so the clip runs beside the solve
    // rather than after it.
    V vsb[2][G][kMaxSubsteps];
    const auto clip = [&](std::uint32_t at, double r, V (*vs)[kMaxSubsteps]) noexcept {
        const V railv(r);
        const V u_per_x(vpu / r);
        for (std::uint32_t gi = 0; gi < groups; ++gi) {
            // V::kLanes, not the local L: MSVC does not take a constexpr
            // local captured by a lambda as a constant expression (C2131).
            const std::uint32_t c0 = gi * V::kLanes;
            for (std::uint32_t j = 0; j < ns; ++j) {
                double xs[V::kLanes];
                for (int l = 0; l < V::kLanes; ++l) xs[l] = in[c0 + l][at + j];
                V x = V::load(xs);
                x = V::select(x.abs() <= finite_max, x, zero);
                // The driver, rail-limited: Vs = rail u / (1 + u^4)^(1/4). |u| is
                // capped first. Past 1e6 the curve is flat at the rail to 1e-6
                // relative; uncapped, u^4 overflowed from |x| of about 1.3e77,
                // and vs collapsed to rail u / inf = 0 (the stage went silent
                // under a huge input instead of clipping); above about 7.2e305
                // x * vpu itself overflowed and vs = inf / inf was NaN.
                V u = x * u_per_x;
                u = V::select(u < V(kUMax), u, V(kUMax));
                u = V::select(u > V(-kUMax), u, V(-kUMax));
                const V u2 = u * u;
                vs[gi][j] = railv * u / (V(1.0) + u2 * u2).sqrt().sqrt();
            }
        }
    };
    if (steps > 0) clip(0, rail_drv_, vsb[0]);
    double rail_drv = rail_drv_;
    for (std::uint32_t t = 0, at = 0; t < steps; ++t, at += ns) {
        if (sag != nullptr) {
            // set_sag's own early return, here: the call (out of line in the
            // pair's file) would spill the lane state on every step.
            double amount = sag[t / steps_per_sag];
            amount = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
            if (amount != sag_) set_sag(amount);
        }
        // Step t + 1's driver, at the rail before this step's draw.
        if (t + 1 < steps) clip(at + ns, rail, vsb[(t + 1) & 1]);
        double drawn = 0.0;
        for (std::uint32_t gi = 0; gi < groups; ++gi) {
            StateV<V>& s = st[gi];
            const std::uint32_t c0 = gi * L;
            const V* vs = vsb[t & 1][gi];
            // The node equation's flux over the step: the driver's share by the
            // trapezoid on every sample, the magnetising current's exactly for
            // the current taken linear across the step (which is how it is read
            // back below). With one substep this is the plain trapezoid.
            V sum = V(0.5) * (s.vs_last + vs[ns - 1]);
            for (std::uint32_t j = 0; j + 1 < ns; ++j) sum = sum + vs[j];
            const V i0 = s.i;
            const V rhs = s.psi + tg * sum - half * i0;

            const V f0 = s.psi - rhs + half * i0;
            const V i = i0 - f0 * s.inv_slope;
            const EvalV<V> e = eval_lanes<V>(k, i0, s.m, s.m_irr, i);
            const V f = e.psi - rhs + half * i;
            const V inv = V(1.0) / (e.dpsi + half);
            const V di = (f * inv).neg();
            const M ok = di.abs() <= di_tol;
            if (ok.all()) {
                s.psi = e.psi + e.dpsi * di;
                s.m = e.m + e.dm * di;
                s.m_irr = e.m_irr + e.dm_irr * di;
                s.i = i + di;
                s.inv_slope = inv;
                if (max_n < 1) max_n = 1;
                evals += L;
            } else {
                double a[5][L];
                for (int l = 0; l < L; ++l) {
                    double il = i.get(l), dil = di.get(l), invl = inv.get(l);
                    EvalV<Lane1> el{Lane1(e.psi.get(l)), Lane1(e.m.get(l)),  Lane1(e.m_irr.get(l)),
                                    Lane1(e.dpsi.get(l)), Lane1(e.dm.get(l)), Lane1(e.dm_irr.get(l))};
                    std::uint32_t n = 1;
                    if (!ok.lane(l)) {
                        Core c;
                        c.i = i0.get(l);
                        c.m = s.m.get(l);
                        c.m_irr = s.m_irr.get(l);
                        double fl = f.get(l);
                        refine(c, rhs.get(l), il, el, fl, invl, dil, n);
                    }
                    if (n > max_n) max_n = n;
                    evals += n;
                    a[0][l] = el.psi.x + el.dpsi.x * dil;
                    a[1][l] = el.m.x + el.dm.x * dil;
                    a[2][l] = el.m_irr.x + el.dm_irr.x * dil;
                    a[3][l] = il + dil;
                    a[4][l] = invl;
                }
                s.psi = V::load(a[0]);
                s.m = V::load(a[1]);
                s.m_irr = V::load(a[2]);
                s.i = V::load(a[3]);
                s.inv_slope = V::load(a[4]);
            }
            nsteps += L;
            s.vs_last = vs[ns - 1];

            const V dis = (s.i - i0) * inv_sub;
            V load(0.0);
            for (std::uint32_t j = 0; j < ns; ++j) {
                const V v = g * vs[j] - rp * (i0 + V(static_cast<double>(j + 1)) * dis);
                // The primary's current, from the driver: push-pull draws it
                // from the rail on both half cycles.
                load = load + (vs[j] - v).abs();
                V y = v * inv_out;
                y = V::select(y.abs() <= finite_max, y, zero);
                for (int l = 0; l < L; ++l) out[c0 + l][at + j] = y.get(l);
            }
            for (int l = 0; l < L; ++l) drawn += load.get(l) * inv_rs * inv_sub_s;
        }
        if (rsup_ <= 1e-9) {
            vres = v_idle;
        } else {
            // Backward Euler on the reservoir (set_sag): stable at any step,
            // however light the source resistance.
            vres = (vres + sup_a_ * (sup_open_ - i_q - drawn)) * sup_inv_den_;
        }
        rail_drv = rail;
        rail = vres - v_knee;
        if (rail < 10.0) rail = 10.0;
    }
    for (std::uint32_t gi = 0; gi < groups; ++gi) {
        for (int l = 0; l < L; ++l) {
            Core& c = ch_[gi * L + l];
            c.psi = st[gi].psi.get(l);
            c.i = st[gi].i.get(l);
            c.m = st[gi].m.get(l);
            c.m_irr = st[gi].m_irr.get(l);
            c.vs_last = st[gi].vs_last.get(l);
            c.inv_slope = st[gi].inv_slope.get(l);
        }
    }
    max_iter_seen_ = max_n;
    evals_ = evals;
    steps_ = nsteps;
    vres_ = vres;
    rail_ = rail;
    rail_drv_ = rail_drv;
}

}  // namespace sonora::dsp
