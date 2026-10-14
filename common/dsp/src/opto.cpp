// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/opto.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

// ---- the cell --------------------------------------------------------------

void OptoCell::prepare(double sample_rate, const OptoCellParams& p) noexcept {
    p_ = p;
    dt_ = 1.0 / sample_rate;
    inv_tau_r_ = 1.0 / p.tau_r;
    // Emission flux per unit occupancy (N_i / tau_i), and capture per unit
    // carrier in occupancy units (c_i / N_i).
    e1_ = p.trap1_density / p.trap1_tau;
    e2_ = p.trap2_density / p.trap2_tau;
    k1_ = p.trap1_capture / p.trap1_density;
    k2_ = p.trap2_capture / p.trap2_density;
    inv_t1_ = 1.0 / p.trap1_tau;
    inv_t2_ = 1.0 / p.trap2_tau;
    floor_v_ = p.el_b * p.el_b * (1.0 / (700.0 * 700.0));
    reset();
}

void OptoCell::reset() noexcept { n_ = nx_ = m1_ = m2_ = 0.0; }

double OptoCell::light(double drive_v) const noexcept {
    const double a = std::fabs(drive_v);
    // exp(-700) is the smallest value kept: below it, a subnormal or zero.
    if (!(a > floor_v_)) return 0.0;
    // b / sqrt(a) as b sqrt(a) / a: the square root and the division
    // start together, which shortens the per-sample chain the side chain
    // sits in (the loop is one chain of dependent operations).
    return std::exp(-p_.el_b * std::sqrt(a) * (1.0 / a));
}

void OptoCell::tick(double generation) noexcept {
    const double dt = dt_;
    const double n_old = n_, m1 = m1_, m2 = m2_;
    // The panel gives at most el_gain (its light is at most 1); bounding it
    // here bounds the carriers for any caller (steady_carriers(el_gain),
    // about 22), which is what keeps the explicit steps below stable.
    const double g = clamp(generation, 0.0, p_.el_gain);
    // Every state one forward-Euler step from the same old state: no
    // division, and the traps off the sample-to-sample chain. The fastest
    // rate any state reaches is trap 1's occupancy, k1 n + 1 / tau_1,
    // about 290 /s at the carriers' bound (the carriers' own, 1 / tau_r +
    // B n + c1 + c2, is about 136 /s): under 0.007 of a step at 44.1 kHz,
    // where the step is stable and positive. Against ngspice's trapezoidal
    // solution (the SPICE reference) at 44.1 kHz: 0.10 dB at worst, in the
    // first millisecond of a 30 dB attack; 0.033 dB or less elsewhere.
    const double loss = inv_tau_r_ + p_.bimolecular * n_old + p_.trap1_capture * (1.0 - m1) +
                        p_.trap2_capture * (1.0 - m2);
    n_ = n_old + dt * (g + e1_ * m1 + e2_ * m2 - loss * n_old);
    // The light of sample k is the midpoint value of the step from
    // t_k - dt/2 to t_k + dt/2, so n_ is the state half a sample before the
    // next audio sample; the conductance that sample sees is extrapolated
    // the half step. Without it the scheme lags by half a sample: 0.13 dB
    // against ngspice in the first millisecond of a hard attack at 48 kHz.
    // Bounded to [n/2, 3n/2], which no drive reaches (the state moves by
    // under 1 % a sample).
    nx_ = clamp(n_ + 0.5 * (n_ - n_old), 0.5 * n_, 1.5 * n_);
    m1_ = clamp(m1 + dt * (k1_ * n_old * (1.0 - m1) - inv_t1_ * m1), 0.0, 1.0);
    m2_ = clamp(m2 + dt * (k2_ * n_old * (1.0 - m2) - inv_t2_ * m2), 0.0, 1.0);
}

double OptoCell::steady_carriers(double generation, const OptoCellParams& p) noexcept {
    if (!(generation > 0.0)) return 0.0;
    const double a = 1.0 / p.tau_r;
    if (!(p.bimolecular > 0.0)) return generation * p.tau_r;
    // The root of B n^2 + a n - G, written without the cancellation of
    // (-a + sqrt(a^2 + 4BG)) / 2B at small G.
    return 2.0 * generation / (a + std::sqrt(a * a + 4.0 * p.bimolecular * generation));
}

// ---- the circuit -----------------------------------------------------------

double peak_reduction_share(double position) noexcept {
    const double p = clamp(position, 0.0, 1.0);
    return (std::exp(p * 4.605170185988092) - 1.0) * (1.0 / 99.0);
}

void OptoChannel::prepare(double sample_rate, const OptoCircuit& c, const OptoCellParams& p) noexcept {
    c_ = c;
    fs_ = sample_rate;
    cell_.prepare(sample_rate, p);
    // C6 into R30 + R2: first-order high-pass, bilinear.
    const double k = 2.0 * sample_rate;
    const double tau = c.c6 * (c.r30 + c.r2);
    hp_b0_ = k * tau / (1.0 + k * tau);
    hp_a1_ = (1.0 - k * tau) / (1.0 + k * tau);
    r37_ = -1.0;  // forces the first update
    set_r37(0.0);
    limit_ = -1.0;
    set_limit(0.0);
    set_peak_share(0.0);
    reset();
}

void OptoChannel::reset() noexcept {
    cell_.reset();
    hp_x1_ = hp_y1_ = 0.0;
    em_x1_ = em_y1_ = 0.0;
}

void OptoChannel::set_peak_share(double share) noexcept {
    peak_share_ = share;
    sc_gain_ = c_.r2 / (c_.r30 + c_.r2) * c_.amp_gain * share;
}

void OptoChannel::set_limit(double limit) noexcept {
    if (limit == limit_) return;
    limit_ = limit;
    r7e_ = c_.r7 * limit;
    // The attenuator's gain with the cell dark, in this switch position.
    const double y = cell_.params().dark_conductance + 1.0 / c_.r1;
    inv_a0_ = 1.0 + (c_.r6 + r7e_) * y;
}

void OptoChannel::set_r37(double ohms) noexcept {
    const double r = clamp(ohms, 0.0, c_.r37_max);
    if (r == r37_) return;
    r37_ = r;
    update_emphasis();
}

void OptoChannel::update_emphasis() noexcept {
    if (!(r37_ > 0.0)) {
        em_b0_ = 1.0;
        em_b1_ = 0.0;
        em_a1_ = 0.0;
        return;
    }
    // R3 / (R3 + R37) (1 + s R37 C12) / (1 + s (R37 || R3) C12), bilinear.
    const double k = 2.0 * fs_;
    const double g = c_.r3 / (c_.r3 + r37_);
    const double tz = r37_ * c_.c12;
    const double tp = c_.r3 * r37_ / (c_.r3 + r37_) * c_.c12;
    const double d = 1.0 / (1.0 + k * tp);
    em_b0_ = g * (1.0 + k * tz) * d;
    em_b1_ = g * (1.0 - k * tz) * d;
    em_a1_ = (1.0 - k * tp) * d;
}

void OptoChannel::divider(const OptoCircuit& c, double limit, double cell_conductance, double dark_conductance,
                          double& gain, double& tap_share) noexcept {
    const double r7e = c.r7 * limit;
    // The shunt's admittance: the cell beside R1. a = Z2 / (R6 + R7 + Z2)
    // with Z2 = 1 / y; the tap is 1 - R6 / (R6 + R7 + Z2) = 1 - R6 y a.
    const double y = cell_conductance + 1.0 / c.r1;
    const double a = 1.0 / (1.0 + (c.r6 + r7e) * y);
    tap_share = 1.0 - c.r6 * y * a;
    gain = a * (1.0 + (c.r6 + r7e) * (dark_conductance + 1.0 / c.r1));
}

void OptoChannel::attenuate(double& gain, double& tap_share) const noexcept {
    // divider(), with the dark gain cached by set_limit().
    const double y = cell_.conductance() + 1.0 / c_.r1;
    const double a = 1.0 / (1.0 + (c_.r6 + r7e_) * y);
    tap_share = 1.0 - c_.r6 * y * a;
    gain = a * inv_a0_;
}

double OptoChannel::sidechain(double tap_v) noexcept {
    const double h = hp_b0_ * (tap_v - hp_x1_) - hp_a1_ * hp_y1_;
    hp_x1_ = tap_v;
    hp_y1_ = h;
    const double e = em_b0_ * h + em_b1_ * em_x1_ - em_a1_ * em_y1_;
    em_x1_ = h;
    em_y1_ = e;
    return sc_gain_ * e;
}

void OptoChannel::drive(double grid_v) noexcept {
    const double lim = c_.drive_limit_v;
    cell_.drive(clamp(grid_v, -lim, lim));
}

double OptoChannel::tick(double x) noexcept {
    double g, tap;
    attenuate(g, tap);
    drive(sidechain(x * c_.full_scale_v * tap));
    return x * g;
}

double OptoChannel::gain_reduction_db() const noexcept {
    double g, tap;
    attenuate(g, tap);
    return -20.0 * std::log10(g);
}

double OptoChannel::sidechain_analog_mag(const OptoCircuit& c, double r37, double f_hz) noexcept {
    const double w = kTwoPi * f_hz;
    const double tau = c.c6 * (c.r30 + c.r2);
    double m = w * tau / std::sqrt(1.0 + w * tau * w * tau);
    if (r37 > 0.0) {
        const double g = c.r3 / (c.r3 + r37);
        const double tz = r37 * c.c12;
        const double tp = c.r3 * r37 / (c.r3 + r37) * c.c12;
        m *= g * std::sqrt((1.0 + w * tz * w * tz) / (1.0 + w * tp * w * tp));
    }
    return m;
}

}  // namespace sonora::dsp
