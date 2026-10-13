// SPDX-License-Identifier: GPL-3.0-or-later
//
// A triode and the common-cathode stage around it, solved per sample from
// the circuit (ADR-0011 §2.1, "triode and pentode models"; §3.3: the
// harmonic profile is what the modelled device produces, never a
// waveshaper tuned to a recipe). Stage N2.
//
// The device, two published laws, both fitted to the 12AX7 high-mu double
// triode by their authors:
// - K. Dempwolf and U. Zölzer, "A physically-motivated triode model for
//   circuit simulations", DAFx-11, Paris, 2011, equations (10) to (12) and
//   Table 1, column "RSD-1":
//     Ik = G (log(1 + exp(C (Vpk / mu + Vgk))) / C)^gamma
//     Ig = Gg (log(1 + exp(Cg Vgk)) / Cg)^xi + Ig0
//     Ip = Ik - Ig
//   Continuously differentiable, with grid current fitted to measurements.
//   The stage uses it by default.
// - N. Koren, "Improved vacuum tube models for SPICE simulations", Glass
//   Audio 8(5), 1996, equation (4) and Table 1:
//     E1 = (Vpk / kp) log(1 + exp(kp (1 / mu + Vgk / sqrt(kvb + Vpk^2))))
//     Ip = (E1^ex / kg1) (1 + sgn(E1))
//   The article gives grid current only as RGI, "the grid-to-cathode
//   resistance that controls grid current when EG > 0"; here that is
//   Ig = max(Vgk, 0) / RGI, and the SPICE reference uses the same law.
// - Interelectrode capacitances: the datasheet values both papers use,
//   grid-cathode 2.3 pF, grid-plate 2.4 pF, plate-cathode 0.9 pF.
//
// The stage (a textbook resistance-coupled amplifier; values ours, chosen
// in the range the tube manuals tabulate for a high-mu triode):
//
//   vin --Cin-- a --Rgrid-- g        B+ --Rplate-- p --Cout-- o
//               |                                             |
//             Rleak         k --Rk||Ck-- gnd                Rload
//
// with Cgk, Cgp and Cpk between the electrodes. Grid current through
// Rgrid and Cin shifts the bias on loud passages ("blocking"), and Cgp
// times the stage gain is the Miller capacitance that rolls off the top:
// both come out of the circuit, not out of a parameter.
//
// Solution method: modified nodal analysis with trapezoidal companion
// models for the capacitors (L. O. Chua and P.-M. Lin, *Computer-Aided
// Analysis of Electronic Circuits*, 1975), reduced to the two nonlinear
// port voltages Vgk and Vpk as in the DK method (D. T. Yeh, J. S. Abel and
// J. O. Smith, "Automated physical modeling of nonlinear audio circuits
// for real-time audio effects, part I", IEEE TASLP 18(4), 2010). Every
// matrix is fixed once the circuit and the rate are, so prepare() inverts
// it; a sample costs about 100 multiply-adds plus a two-dimensional Newton
// solve, started from a predictor (one step with last sample's Jacobian)
// and bounded at kMaxIterations, so the worst case is fixed. Measured:
// 1 to 3 iterations up to grid current, 5 at 63 V peak; the bound is
// reached only by absurd inputs (1000 V), whose output stays finite. The
// trapezoidal rule matches SPICE's own default integration,
// which is what the reference harness (plugins/native/measure, spice.h)
// compares against.
//
// Run it oversampled (oversampling.h): the device generates harmonics
// without limit, and this solver has no anti-aliasing of its own.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp {

enum class TriodeLaw : std::uint8_t { Dempwolf, Koren };

// Device parameters. Defaults: 12AX7, from the papers cited above.
struct TriodeDevice {
    // Dempwolf and Zölzer, DAFx-11, Table 1, "RSD-1".
    double g = 2.242e-3, mu = 103.2, gamma = 1.26, c = 3.40;
    double gg = 6.177e-4, xi = 1.314, cg = 9.901, ig0 = 8.025e-8;
    // Koren, Glass Audio 1996, Table 1, 12AX7.
    double k_mu = 100.0, k_ex = 1.4, k_kg1 = 1060.0, k_kp = 600.0, k_kvb = 300.0, k_rgi = 2000.0;
    // Interelectrode capacitances, farads.
    double c_gk = 2.3e-12, c_gp = 2.4e-12, c_pk = 0.9e-12;
};

// Plate and grid current, each flowing into the cathode, and their partial
// derivatives in the port voltages (amperes, volts).
struct TriodeCurrents {
    double ip = 0.0, ig = 0.0;
    double dip_dvgk = 0.0, dip_dvpk = 0.0, dig_dvgk = 0.0;
};

// The published equations, evaluated directly.
TriodeCurrents triode_currents(TriodeLaw law, const TriodeDevice& d, double vgk, double vpk) noexcept SONORA_RT;

// Component values, ohms, farads, volts.
struct TriodeStageCircuit {
    double supply_v = 250.0;
    double r_plate = 100e3;
    double r_cathode = 1.5e3;
    double c_cathode = 22e-6;
    double c_in = 22e-9;
    double r_leak = 1e6;
    double r_grid = 22e3;
    double c_out = 22e-9;
    double r_load = 1e6;
};

class TriodeStage {
public:
    static constexpr std::uint32_t kNodes = 5;  // a, g, k, p, o
    static constexpr std::uint32_t kCaps = 6;   // Cin, Ck, Cout, Cgk, Cgp, Cpk
    enum Node : std::uint32_t { kA = 0, kG, kK, kP, kO };
    // Rows of the solver's packed linear part (see w_ below): 9, padded.
    static constexpr std::uint32_t kRows = 10;

    // Not real-time: builds and inverts the matrices, tabulates the
    // Dempwolf law (when `tabulated`), and solves the operating point.
    // Returns false when the operating point does not converge.
    bool prepare(double sample_rate, TriodeLaw law = TriodeLaw::Dempwolf,
                 const TriodeStageCircuit& circuit = {}, const TriodeDevice& device = {},
                 bool tabulated = true);
    // Back to the operating point, as if the input had always been 0 V.
    void reset() noexcept;

    // One sample: the input voltage in, the voltage across the load out.
    // Inverting: a positive input swings the plate down.
    double tick(double vin) noexcept SONORA_RT;
    // Two stages, one sample each, solved in lockstep: the same results as
    // a.tick(vin_a) and b.tick(vin_b), to the bit, in less time than two
    // ticks (the stereo processor's path).
    static void tick2(TriodeStage& a, TriodeStage& b, double vin_a, double vin_b, double& out_a,
                      double& out_b) noexcept SONORA_RT;

    // The operating point (volts, amperes), from prepare().
    double op_voltage(Node n) const noexcept { return op_v_[n]; }
    double op_plate_current() const noexcept { return op_i_[0]; }
    // Small-signal voltage gain from vin to the load at f_hz, linearised at
    // the operating point (complex nodal analysis, continuous time):
    // magnitude, and phase in degrees. Not real-time.
    void small_signal_gain(double f_hz, double& magnitude, double& phase_deg) const;

    // Newton iterations of the last tick() and the most any tick took
    // since prepare() or reset(). For tests and reports.
    std::uint32_t last_iterations() const noexcept { return last_iter_; }
    std::uint32_t max_iterations_seen() const noexcept { return max_iter_; }
    static constexpr std::uint32_t kMaxIterations = 12;

    // The device law as the stage evaluates it (the table when tabulated).
    TriodeCurrents currents(double vgk, double vpk) const noexcept SONORA_RT;

private:
    // Cubic Hermite interpolant of f(x) from f and f' on a uniform grid,
    // stored as the four polynomial coefficients of each interval so a
    // lookup is one index and two short Horner chains; the exact law
    // outside the grid (h = 1/128 V: the error is under 1e-12 A, measured
    // by test_triode).
    struct Table {
        double lo = 0.0, hi = 0.0, inv_h = 0.0;
        std::vector<double> c;  // 4 per interval: y = c0 + s (c1 + s (c2 + s c3))
        bool eval(double x, double& y, double& dy) const noexcept SONORA_RT;
    };

    void build_dc_point_matrices(double s_dc[kNodes][kNodes]) const;
    // currents(), inlined into the solver.
    TriodeCurrents law(double vgk, double vpk) const noexcept SONORA_RT;
    template <int L>
    static void step(TriodeStage* const* st, const double* vin, double* out) noexcept SONORA_RT;

    TriodeLaw law_ = TriodeLaw::Dempwolf;
    TriodeDevice dev_;
    TriodeStageCircuit ckt_;
    bool tabulated_ = false;
    Table ik_, ig_;  // Dempwolf: Ik(u), u = Vpk / mu + Vgk; Ig(Vgk)
    double fs_ = 48000.0;

    // Per sample, everything linear in (J, vin, 1) is one product
    // base = W x, stored column by column so the compiler can run it as
    // packed multiply-adds without reassociating a sum (no fast-math):
    //   base[0..1]  p, the port voltages before the devices: v = p + K i
    //   base[2]     the load voltage before the devices
    //   base[3..8]  the next history sources J' before the devices
    // and the devices' currents add K i, Oi . i and Ai i after the solve.
    static constexpr std::uint32_t kCols = kCaps + 2;   // J, vin, 1
    alignas(16) double w_[kCols][kRows] = {};
    alignas(16) double post_[2][kRows] = {};            // column ip, column ig
    double k_[2][2] = {};

    double j_[kCaps] = {};
    double op_j_[kCaps] = {};
    double op_v_[kNodes] = {};
    double op_i_[2] = {};       // ip, ig at the operating point
    double op_port_[2] = {};    // vgk, vpk at the operating point
    // Last sample's solution, currents and Jacobian (dip/dvgk, dip/dvpk,
    // dig/dvgk): the predictor.
    double v_[2] = {}, i_[2] = {}, ji_[3] = {};
    double op_ji_[3] = {};
    // (I - K Ji)^-1 at the last solution, row-major: the predictor's.
    double inv_[4] = {1.0, 0.0, 0.0, 1.0}, op_inv_[4] = {1.0, 0.0, 0.0, 1.0};
    double inv_mu_ = 1.0;
    std::uint32_t last_iter_ = 0, max_iter_ = 0;
};

}  // namespace sonora::dsp
