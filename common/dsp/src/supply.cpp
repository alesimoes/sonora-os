// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/supply.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

// 3x3 inverse by cofactors; the filter's matrix is symmetric positive
// definite, so it always exists.
void invert3(const double a[3][3], double out[3][3]) {
    const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    out[0][0] = c00 / det;
    out[1][0] = c01 / det;
    out[2][0] = c02 / det;
    out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
}

}  // namespace

bool simulate_supply(const SupplyCircuit& c, std::uint32_t harmonics, SupplyRipple& out,
                     std::uint32_t steps_per_period, std::uint32_t settle_periods) {
    out = SupplyRipple{};
    out.ripple_hz = 2.0 * c.mains_hz;
    if (!(c.mains_hz > 0.0) || steps_per_period < 64 || harmonics == 0) return false;
    const double h = 1.0 / (c.mains_hz * steps_per_period);
    const double w_mains = kTwoPi * c.mains_hz;
    const double vt2 = 2.0 * c.diode_n * kBoltzmann * kNoiseTemperatureK / kElectronCharge;  // two diodes

    // Nodes: reservoir, filter, stage. (C/h + G/2) V' = (C/h - G/2) V + s
    // + e1 (i_d' + i_d) / 2, with s the constant loads.
    const double cap[3] = {c.c_reservoir, c.c_filter1, c.c_filter2};
    const double g1 = 1.0 / c.r_filter1, g2 = 1.0 / c.r_filter2;
    const double g[3][3] = {{g1, -g1, 0.0}, {-g1, g1 + g2, -g2}, {0.0, -g2, g2}};
    double a[3][3], b[3][3], ainv[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            const double cd = r == k ? cap[r] / h : 0.0;
            a[r][k] = cd + 0.5 * g[r][k];
            b[r][k] = cd - 0.5 * g[r][k];
        }
    }
    invert3(a, ainv);
    const double s[3] = {-c.i_output, 0.0, -c.i_preamp};
    const double rt = c.r_winding + 0.5 * ainv[0][0];

    // Start near the DC point the loads imply. The reservoir settles in a
    // few of its own (short) time constants; the filter's slow mode (about
    // 0.5 s) would take hundreds of periods to decay, and while it does it
    // moves the reservoir's load and so its ripple (0.4 % after 40
    // periods, measured against ngspice). So through the first half of
    // the settling, at the end of each period, the filter's nodes are
    // shifted to the DC their load implies from the reservoir's mean over
    // that period, which leaves the slow mode nothing to decay.
    const double i_total = c.i_output + c.i_preamp;
    double v[3];
    v[0] = c.secondary_v_peak - vt2 * std::log1p(10.0 * i_total / c.diode_is) - 0.02 * c.secondary_v_peak;
    v[1] = v[0] - c.r_filter1 * c.i_preamp;
    v[2] = v[1] - c.r_filter2 * c.i_preamp;
    double i_d = 0.0;
    double w = 0.0;

    const std::uint32_t total = (settle_periods + 1) * steps_per_period;
    const std::uint32_t from = settle_periods * steps_per_period;
    out.reservoir.assign(harmonics, {0.0, 0.0});
    double dc = 0.0;
    double mean[3] = {0.0, 0.0, 0.0};
    for (std::uint32_t n = 1; n <= total; ++n) {
        double rhs[3];
        for (int r = 0; r < 3; ++r) rhs[r] = b[r][0] * v[0] + b[r][1] * v[1] + b[r][2] * v[2] + s[r];
        rhs[0] += 0.5 * i_d;
        double q[3];
        for (int r = 0; r < 3; ++r) q[r] = ainv[r][0] * rhs[0] + ainv[r][1] * rhs[1] + ainv[r][2] * rhs[2];
        // The bridge: |vs| - V1' = i Rw + 2 n Vt ln(1 + i / Is), with
        // V1' = q1 + ainv11 i / 2. In w = ln(1 + i / Is),
        //   f(w) = Is (e^w - 1) Rt + 2 n Vt w - (|vs| - q1) = 0,
        // increasing and convex, so Newton started right of the root falls
        // to it monotonically. ln(1 + c / (Is Rt)) is right of it when the
        // bridge conducts (c > 0), and 0 when it does not.
        const double t = static_cast<double>(n) * h;
        const double target = std::fabs(c.secondary_v_peak * std::sin(w_mains * t)) - q[0];
        w = target > 0.0 ? std::log1p(target / (c.diode_is * rt)) : 0.0;
        std::uint32_t it = 0;
        for (;; ++it) {
            if (it == 100) return false;
            const double e = std::exp(w);
            const double f = c.diode_is * (e - 1.0) * rt + vt2 * w - target;
            const double df = c.diode_is * e * rt + vt2;
            const double step = f / df;
            w -= step;
            if (std::fabs(step) <= 1e-13 * (1.0 + std::fabs(w))) break;
        }
        if (it > out.max_newton) out.max_newton = it;
        i_d = c.diode_is * std::expm1(w);
        for (int r = 0; r < 3; ++r) v[r] = q[r] + 0.5 * ainv[r][0] * i_d;
        if (!std::isfinite(v[0]) || !std::isfinite(v[2])) return false;
        for (int r = 0; r < 3; ++r) mean[r] += v[r];
        if (n % steps_per_period == 0) {
            if (n <= from / 2) {
                const double m0 = mean[0] / steps_per_period;
                v[1] += m0 - c.r_filter1 * c.i_preamp - mean[1] / steps_per_period;
                v[2] += m0 - (c.r_filter1 + c.r_filter2) * c.i_preamp - mean[2] / steps_per_period;
            }
            mean[0] = mean[1] = mean[2] = 0.0;
        }
        if (n > from) {
            // One mains period: harmonic k of the ripple is harmonic
            // 2 (k + 1) of the mains, against e^{-j w t} at absolute t.
            dc += v[0];
            for (std::uint32_t k = 0; k < harmonics; ++k) {
                const double wk = w_mains * 2.0 * (k + 1);
                out.reservoir[k] += v[0] * std::polar(1.0, -wk * t);
            }
        }
    }
    const double inv = 1.0 / static_cast<double>(steps_per_period);
    for (auto& z : out.reservoir) z *= 2.0 * inv;
    out.reservoir_dc = dc * inv;
    out.node_dc = out.reservoir_dc - (c.r_filter1 + c.r_filter2) * c.i_preamp;
    return true;
}

std::complex<double> supply_filter_transfer(const SupplyCircuit& c, double f_hz, std::complex<double> y_load) {
    using C = std::complex<double>;
    const C jw(0.0, kTwoPi * f_hz);
    const C z3 = 1.0 / (jw * c.c_filter2 + y_load);
    const C t3 = z3 / (c.r_filter2 + z3);
    const C z2 = 1.0 / (jw * c.c_filter1 + 1.0 / (c.r_filter2 + z3));
    const C t2 = z2 / (c.r_filter1 + z2);
    return t2 * t3;
}

}  // namespace sonora::dsp
