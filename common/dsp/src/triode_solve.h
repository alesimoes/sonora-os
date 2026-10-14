// SPDX-License-Identifier: GPL-3.0-or-later
//
// The triode stage's per-sample solve, written once over a lane type V so
// one stage (tick(), V of one double, in triode.cpp) and two stages in
// lockstep (tick2(), V of two doubles, in src/arch/<arch>/triode_pair.cpp)
// run the same operations in the same order: a stage's output does not
// depend on whether it ran alone or beside another (test_triode checks it
// to the bit). On x86-64 the two-lane V is SSE2, which is IEEE double
// arithmetic like the scalar unit, so packing changes the speed and not
// the bits. Internal to common/dsp.
//
// V provides: V::kLanes; V(double) broadcast; V::dup(const double*), a
// broadcast from a pre-duplicated pair; V::load(const double*);
// V::set(const double*), the same from values just computed in registers;
// store(double*); get(lane); + - * /; neg; abs; max(a, b) = a > b ? a : b;
// comparisons returning V::Mask; select(mask, a, b); and Mask with & | ~,
// lane(i), all(), any().
#pragma once

#include <cmath>
#include <cstdint>

#include "sonora/dsp/triode.h"

namespace sonora::dsp {

namespace triode_detail {

// Where the tables end, |C x| >= 36: there log(1 + e^(Cx)) is e^(Cx), or
// Cx, to the last bit of a double (e^-36 is 2e-16 of 1, under half an ulp
// of 36), so each law is one exp or one pow and still exact. Cut-off far
// below the bias and heavy grid current on a hot input land here every
// sample; the full softplus form there cost four transcendental calls per
// evaluation (the RT audit of PR #52).
//
// Deep in cut-off (Cx < -100) the current is under 1e-46 A, which no sum
// it enters can see (the grid's Ig0 alone is 8e-8 A, whose last bit is
// 1e-23 A): it is returned as 0 without calling exp, so a hot input that
// holds the grid at -60 V costs no transcendental call per pass.
inline void tail_ik(const TriodeDevice& d, double u, double& y, double& dy) noexcept {
    const double x = d.c * u;
    if (x < -100.0) {
        y = dy = 0.0;
    } else if (x < 0.0) {
        // G (e^(Cu) / C)^gamma
        y = d.g * std::exp(d.gamma * (x - std::log(d.c)));
        dy = d.gamma * d.c * y;
    } else {
        // G u^gamma
        const double p = std::pow(u, d.gamma - 1.0);
        y = d.g * p * u;
        dy = d.g * d.gamma * p;
    }
}

inline void tail_ig(const TriodeDevice& d, double v, double& y, double& dy) noexcept {
    const double x = d.cg * v;
    if (x < -100.0) {
        y = d.ig0;
        dy = 0.0;
    } else if (x < 0.0) {
        const double g = d.gg * std::exp(d.xi * (x - std::log(d.cg)));
        y = g + d.ig0;
        dy = d.xi * d.cg * g;
    } else {
        const double p = std::pow(v, d.xi - 1.0);
        y = d.gg * p * v + d.ig0;
        dy = d.gg * d.xi * p;
    }
}

}  // namespace triode_detail

inline bool TriodeStage::Table::eval(double x, double& y, double& dy) const noexcept {
    if (!(x >= lo && x < hi)) return false;
    const double t = (x - lo) * inv_h;
    // x just under hi can round t to the last node: stay in the last
    // interval (s = 1 there, the node's own value).
    int i = static_cast<int>(t);
    i = i > last ? last : i;
    const double s = t - static_cast<double>(i);
    const double* k = &c[static_cast<std::size_t>(i) * 4];
    // Estrin's scheme rather than Horner's: the lookup sits on the solve's
    // chain of dependent operations, and pairing the terms shortens it.
    const double s2 = s * s;
    y = (k[0] + k[1] * s) + (k[2] + k[3] * s) * s2;
    dy = ((k[1] + (2.0 * k[2]) * s) + (3.0 * k[3]) * s2) * inv_h;
    return true;
}

inline TriodeCurrents TriodeStage::law(double vgk, double vpk) const noexcept {
    if (!tabulated_) return triode_currents(law_, dev_, vgk, vpk);
    double ik, dik, ig, dig;
    const double u = vpk * inv_mu_ + vgk;
    if (!ik_.eval(u, ik, dik)) triode_detail::tail_ik(dev_, u, ik, dik);
    if (!ig_.eval(vgk, ig, dig)) triode_detail::tail_ig(dev_, vgk, ig, dig);
    TriodeCurrents r;
    r.ip = ik - ig;
    r.ig = ig;
    r.dip_dvgk = dik - dig;
    r.dip_dvpk = dik * inv_mu_;
    r.dig_dvgk = dig;
    return r;
}

// One sample of V::kLanes stages that share their matrices (prepared with
// the same rate, law, circuit and device: tick2() checks model_key_).
//
// A sample is one chain of dependent operations: last sample's capacitor
// histories J -> the port voltages p -> the predictor -> the law -> the
// Newton steps -> the new J. Its length, not the arithmetic, sets the cost
// (measured with perf), so the linear sums are trees rather than running
// sums, and the part of the new J that does not depend on the solve is
// formed before it, off the chain.
template <class V>
void TriodeStage::solve(TriodeStage* const* st, const double* vin_in, double* out) noexcept {
    constexpr int L = V::kLanes;
    constexpr std::uint32_t M = kCaps;
    using Mask = typename V::Mask;
    const TriodeStage& m0 = *st[0];  // the shared matrices

    // Silence at rest is rest: a stage that has seen nothing but 0 since
    // reset() keeps its state to the bit and puts out its rest value. The
    // operating point is the solve's own only to its tolerance, so without
    // this every sample of a host's preroll of silence would move the
    // state in its last bits, and what follows the preroll would differ
    // from the same input without it (by 1e-8 at the output).
    bool hold[L];
    bool all_hold = true;
    for (int l = 0; l < L; ++l) {
        hold[l] = st[l]->at_rest_ && vin_in[l] == 0.0;
        all_hold = all_hold && hold[l];
    }
    if (all_hold) {
        for (int l = 0; l < L; ++l) {
            st[l]->last_iter_ = 0;
            out[l] = st[l]->rest_out_;
        }
        return;
    }

    // The input, bounded to what a source can put across Cin
    // (kMaxInputV); NaN, which the processor never passes, reads as 0.
    double lanes[L];
    for (int l = 0; l < L; ++l) {
        double x = vin_in[l];
        x = x > kMaxInputV ? kMaxInputV : (x < -kMaxInputV ? -kMaxInputV : x);
        lanes[l] = x == x ? x : 0.0;
    }
    const V vin = V::load(lanes);
    V j[M];
    for (std::uint32_t m = 0; m < M; ++m) {
        for (int l = 0; l < L; ++l) lanes[l] = st[l]->j_[m];
        j[m] = V::load(lanes);
    }
    // One value of each lane's state, as a V.
    const auto gather = [&](auto pick) {
        for (int l = 0; l < L; ++l) lanes[l] = pick(*st[l]);
        return V::load(lanes);
    };

    static_assert(M == 4, "the sums below are written for four capacitors");
    // The coefficients come pre-broadcast (dup_): one load each, not a
    // load and a shuffle.
    const auto c = [&](std::uint32_t i) { return V::dup(m0.dup_[i]); };
    // One row of W (J, vin, 1), as a tree: (1, vin) + ((J0, J1) + (J2, J3)).
    const auto row = [&](std::uint32_t at) {
        const V a = c(at + M + 1) + c(at + M) * vin;
        const V b = c(at) * j[0] + c(at + 1) * j[1];
        const V d = c(at + 2) * j[2] + c(at + 3) * j[3];
        return a + (b + d);
    };
    // p, the port voltages before the devices: v = p + K i.
    const V p0 = row(kDupP);
    const V p1 = row(kDupP + kCols);
    // The load and the next histories before the devices' currents.
    V base[1 + M];
    for (std::uint32_t r = 0; r < 1 + M; ++r) base[r] = row(kDupW + r * kCols);
    const V k00 = c(kDupK), k01 = c(kDupK + 1), k10 = c(kDupK + 2), k11 = c(kDupK + 3);

    // Predictor: one Newton step from last sample's solution with last
    // sample's Jacobian and its inverse, which costs no evaluation of the
    // law and no division.
    const V sv0 = gather([](const TriodeStage& s) { return s.v_[0]; });
    const V sv1 = gather([](const TriodeStage& s) { return s.v_[1]; });
    const V si0 = gather([](const TriodeStage& s) { return s.i_[0]; });
    const V si1 = gather([](const TriodeStage& s) { return s.i_[1]; });
    V gpg = gather([](const TriodeStage& s) { return s.ji_[0]; });
    V gpp = gather([](const TriodeStage& s) { return s.ji_[1]; });
    V ggg = gather([](const TriodeStage& s) { return s.ji_[2]; });
    V inv0 = gather([](const TriodeStage& s) { return s.inv_[0]; });
    V inv1 = gather([](const TriodeStage& s) { return s.inv_[1]; });
    V inv2 = gather([](const TriodeStage& s) { return s.inv_[2]; });
    V inv3 = gather([](const TriodeStage& s) { return s.inv_[3]; });
    const V e0 = si0 - (gpg * sv0 + gpp * sv1);
    const V e1 = si1 - ggg * sv0;
    const V r0 = p0 + (k00 * e0 + k01 * e1);
    const V r1 = p1 + (k10 * e0 + k11 * e1);
    V v0 = inv0 * r0 + inv1 * r1;
    V v1 = inv2 * r0 + inv3 * r1;
    // Across a change of regime the last Jacobian is the wrong one: from
    // cut-off (no transconductance) a rising input is predicted to drive
    // the grid volts positive with nothing to hold it, and from grid
    // current (a stiff grid) a falling one is predicted to barely move it;
    // either took the solve 5 to 7 passes back (measured, on a full-drive
    // input). There a linearisation at a fixed point of the right regime
    // predicts instead: the operating point, or, where a prediction without
    // grid current puts the grid above +1 V (which grid current never
    // lets it reach at these values), a point in grid current.
    {
        const auto at = [&](std::uint32_t k, V& w0, V& w1) {
            const V q0 = p0 + c(k), q1 = p1 + c(k + 1);
            w0 = c(k + 2) * q0 + c(k + 3) * q1;
            w1 = c(k + 4) * q0 + c(k + 5) * q1;
        };
        V w0, w1;
        at(kDupOp, w0, w1);
        const Mask cut = gpg < V(kCutoffS);
        const Mask grid = ggg > V(kGridS);
        const Mask swap = (cut & (v0 > c(kDupOp + 6))) | (grid & (v0 < V(0.0)));
        v0 = V::select(swap, w0, v0);
        v1 = V::select(swap, w1, v1);
        const Mask into_grid = ~grid & (v0 > V(1.0));
        if (into_grid.any()) {
            at(kDupGrid, w0, w1);
            v0 = V::select(into_grid, w0, v0);
            v1 = V::select(into_grid, w1, v1);
        }
    }
    V i0 = si0, i1 = si1;

    const V one(1.0), max_step(kMaxStepV), tol0(1e-4), tol1(1e-2);
    Mask conv = V::none();
    std::uint32_t it[L] = {};
    for (std::uint32_t pass = 0; pass < kMaxIterations; ++pass) {
        // The law, lane by lane (a table lookup each); a converged lane
        // keeps its values.
        double cip[L], cig[L], cpg[L], cpp[L], cgg[L];
        for (int l = 0; l < L; ++l) {
            if (conv.lane(l)) {
                cip[l] = i0.get(l);
                cig[l] = i1.get(l);
                cpg[l] = gpg.get(l);
                cpp[l] = gpp.get(l);
                cgg[l] = ggg.get(l);
                continue;
            }
            ++it[l];
            const TriodeCurrents c = st[l]->law(v0.get(l), v1.get(l));
            cip[l] = c.ip;
            cig[l] = c.ig;
            cpg[l] = c.dip_dvgk;
            cpp[l] = c.dip_dvpk;
            cgg[l] = c.dig_dvgk;
        }
        // Built from registers, not through memory (V::set).
        const V Cip = V::set(cip), Cig = V::set(cig), Cpg = V::set(cpg), Cpp = V::set(cpp), Cgg = V::set(cgg);
        const V f0 = (p0 - v0) + (k00 * Cip + k01 * Cig);
        const V f1 = (p1 - v1) + (k10 * Cip + k11 * Cig);
        // JF = K Ji - I; -JF = I - K Ji, whose inverse is the next sample's
        // predictor.
        const V j00 = k00 * Cpg + k01 * Cgg - one;
        const V j01 = k00 * Cpp;
        const V j10 = k10 * Cpg + k11 * Cgg;
        const V j11 = k10 * Cpp - one;
        // Cramer's rule, each step divided by det directly: one division
        // on the chain rather than a reciprocal and a product.
        const V det = j00 * j11 - j01 * j10;
        V d0 = (f0.neg() * j11 + f1 * j01) / det;
        V d1 = (f1.neg() * j00 + f0 * j10) / det;
        // A bounded step, against an absurd Jacobian only. It is not
        // damping: the plate legitimately moves hundreds of volts in a
        // solve on a hot edge, and a 20 or 50 V cap was measured to stall
        // the solve there (12 iterations unconverged on a 63 V square at
        // 2x), while the undamped solve converged in at most 7 on every
        // input test_triode drives up to kMaxInputV.
        // Rare, so a branch keeps it off the chain of the common pass.
        const V big = V::max(d0.abs(), d1.abs());
        const Mask too_big = big > max_step;
        if (too_big.any()) {
            const V scale = V::select(too_big, max_step / big, one);
            d0 = d0 * scale;
            d1 = d1 * scale;
        }
        const Mask active = ~conv;
        // det can only be non-finite, or zero, from a non-finite state; such
        // a lane stops here, unconverged, and is restarted below.
        const V rdet = one / det;
        const Mask finite = (rdet == rdet) & ((rdet - rdet) == V(0.0));
        // Newton converges quadratically: after a step of 1e-4 V the error
        // left at the grid is of order 1e-7 V (the laws' curvature is at
        // most ~10 /V), about -150 dB at the processor's output. The
        // plate moves ~100 times the grid, so its threshold is 100 times
        // wider.
        const Mask now = active & finite & (d0.abs() < tol0) & (d1.abs() < tol1);
        v0 = V::select(active, v0 + d0, v0);
        v1 = V::select(active, v1 + d1, v1);
        // The currents at the new point, to first order: the step is the
        // Newton step, so this is exact to the square of the step.
        i0 = V::select(active, Cip + Cpg * d0 + Cpp * d1, i0);
        i1 = V::select(active, Cig + Cgg * d0, i1);
        gpg = Cpg;
        gpp = Cpp;
        ggg = Cgg;
        // (I - K Ji)^-1 = -JF^-1
        inv0 = V::select(now, j11.neg() * rdet, inv0);
        inv1 = V::select(now, j01 * rdet, inv1);
        inv2 = V::select(now, j10 * rdet, inv2);
        inv3 = V::select(now, j00.neg() * rdet, inv3);
        const Mask stopped = active & ~finite;
        for (int l = 0; l < L; ++l) {
            if (stopped.lane(l)) it[l] = kMaxIterations + 1;
        }
        conv = conv | now | stopped;
        if (conv.all()) break;
    }

    // The load voltage and the next capacitor histories: base + Post i.
    for (std::uint32_t r = 0; r < 1 + M; ++r)
        base[r] = base[r] + (c(kDupPost + 2 * r) * i0 + c(kDupPost + 2 * r + 1) * i1);

    double ov0[L], ov1[L], oi0[L], oi1[L], og0[L], og1[L], og2[L], on0[L], on1[L], on2[L], on3[L];
    double ob[1 + M][L];
    v0.store(ov0);
    v1.store(ov1);
    i0.store(oi0);
    i1.store(oi1);
    gpg.store(og0);
    gpp.store(og1);
    ggg.store(og2);
    inv0.store(on0);
    inv1.store(on1);
    inv2.store(on2);
    inv3.store(on3);
    for (std::uint32_t r = 0; r < 1 + M; ++r) base[r].store(ob[r]);
    const double supply = m0.ckt_.supply_v;
    for (int l = 0; l < L; ++l) {
        TriodeStage& s = *st[l];
        if (hold[l]) {
            s.last_iter_ = 0;
            out[l] = s.rest_out_;
            continue;
        }
        s.at_rest_ = false;
        s.last_iter_ = it[l];
        if (it[l] > s.max_iter_) s.max_iter_ = it[l];
        // Unconverged, or at a voltage no stage on this supply can reach:
        // a state Newton could not hold. Left in place it would feed the
        // capacitor histories and run away (the RT audit of PR #52 saw
        // 5e37 V at +12 dBFS and +36 dB of drive), so the stage restarts
        // from its operating point, which costs one bounded reset.
        const bool sane = conv.lane(l) && it[l] <= kMaxIterations && std::isfinite(oi0[l]) &&
                          std::isfinite(oi1[l]) && std::isfinite(ob[0][l]) && std::fabs(ov0[l]) <= supply &&
                          ov1[l] >= -supply && ov1[l] <= 2.0 * supply;
        if (!sane) {
            s.reset();
            ++s.resets_;
            out[l] = 0.0;
            continue;
        }
        s.v_[0] = ov0[l];
        s.v_[1] = ov1[l];
        s.i_[0] = oi0[l];
        s.i_[1] = oi1[l];
        s.ji_[0] = og0[l];
        s.ji_[1] = og1[l];
        s.ji_[2] = og2[l];
        s.inv_[0] = on0[l];
        s.inv_[1] = on1[l];
        s.inv_[2] = on2[l];
        s.inv_[3] = on3[l];
        out[l] = ob[0][l];
        for (std::uint32_t m = 0; m < M; ++m) s.j_[m] = ob[1 + m][l];
    }
}

}  // namespace sonora::dsp
