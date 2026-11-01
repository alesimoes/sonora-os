// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage of a tube amplifier: a push-pull driver working from a
// sagging supply into an output transformer with a hysteretic iron core
// (ADR-0011 §3.3: "tubes, transformers, tape hysteresis", from the
// circuit). Stage N2, slice 3.
//
//     Vs ---- Rs ----+---- node v ----+---- RL (the load, referred to the
//   (driver,         |                |            primary)
//    rail-limited)  Lm(i): the magnetising branch, a nonlinear inductor
//
// - The driver is a voltage Vs behind the source resistance Rs (the
//   plates' dynamic resistance, referred). It swings up to the supply
//   rail and no further: Vs = x / (1 + (x / rail)^4)^(1/4), a smooth
//   stand-in for the load-line clipping of a class-AB pair (a
//   simplification of ours: odd, unity at small signal, asymptote at the
//   rail, and a knee sharper than a tanh's, which at half the rail
//   compresses 0.5 % where a tanh would 4 %, so the mid-band stays clean).
// - The supply is a reservoir C behind a source resistance
//   (rectifier and winding), feeding a quiescent current and the
//   primary's current, rectified (push-pull draws on each half cycle). A
//   loud passage pulls the reservoir down, the rail with it, and the
//   headroom shrinks; the reservoir recovers with tau = Rsup C. This is
//   the sag a one-stage preamp is too light a load to show (slice 2).
// - The core is the Jiles-Atherton model (D. C. Jiles and D. L. Atherton,
//   "Theory of ferromagnetic hysteresis", J. Magn. Magn. Mater. 61, 1986;
//   the irreversible-magnetisation update and its non-physical-region
//   guard after Jiles, Thoelke and Devine, IEEE Trans. Magn. 28(1), 1992).
//   Written in amperes: H is the magnetising current i, M the
//   magnetisation in the same unit, and the flux linkage is
//       psi = Lc (i + M(i)),
//   Lc being the winding's inductance without the core. The anhysteretic
//   curve is Ms L((H + alpha M) / a), L the Langevin function.
//
// The magnetising branch is driven through the node equation
//   v = g Vs - Rp i,   g = RL / (Rs + RL),  Rp = Rs || RL,
//   d psi / dt = v,
// integrated with the trapezoidal rule at the oversampled rate. Each
// sample solves one scalar equation for i. Its residual is strictly
// increasing in i (psi(i) does not decrease, and -Rp i adds a positive
// slope), so the root is bracketed from one evaluation and a safeguarded
// secant converges in a few iterations; the bound is reported
// (max_iterations_seen) and held by a test.
//
// Why it is "LF first": the flux is the integral of the voltage, so for
// a given level it grows as 1 / f. The core saturates on bass first, the
// hysteresis loop is widest there, and the magnetising current (the
// distortion) rises as the frequency falls. The mid-band is nearly
// linear. Measured in common/dsp/tests/test_transformer.cpp.
//
// Real-time safe: no allocation after prepare(), bounded iterations, finite
// output for any finite input (non-finite input is read as 0).
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::dsp {

struct TransformerCircuit {
    // The signal in the processor is 1.0 = this many volts at the driver.
    double volts_per_unit = 250.0;
    double r_source = 1500.0;  // Rs, ohms
    double r_load = 3000.0;    // RL, ohms, referred to the primary
    // The supply: unloaded voltage is chosen so that the rail at idle is
    // v_rail_idle whatever the source resistance; sag follows from r_supply.
    double v_rail_idle = 311.0;
    double v_knee = 40.0;       // the driver's drop to the rail, volts
    double i_quiescent = 0.060;
    double r_supply = 600.0;    // rectifier and winding, at sag = 100 %
    double c_reservoir = 100e-6;
};

struct CoreParams {
    double l_core = 0.01;   // Lc, henries: the winding without the core
    double m_sat = 80.0;    // Ms, amperes-equivalent: Lc Ms is the saturation flux, volt-seconds
    double a = 0.02;        // shape of the anhysteretic curve, amperes
    double k = 0.006;       // coercivity, amperes
    double c = 0.3;         // reversible fraction
    double alpha = 2e-5;    // domain coupling
};

class OutputStage {
public:
    static constexpr std::uint32_t kMaxChannels = 2;
    // The bound the suite holds the solve to.
    static constexpr std::uint32_t kMaxIter = 24;

    // Not real-time. Returns false for a non-positive rate, a channel count
    // outside 1..kMaxChannels or a circuit that is not physical.
    bool prepare(double sample_rate, std::uint32_t channels, const TransformerCircuit& c = {},
                 const CoreParams& p = {});
    // Rest: core demagnetised, reservoir at its idle voltage.
    void reset() noexcept SONORA_RT;

    // 0 = a stiff supply (the rail never moves), 1 = the full source
    // resistance. May change every sample; the reservoir state is continuous.
    void set_sag(double amount) noexcept SONORA_RT;

    // One sample on every channel: `in` is the signal (1.0 = volts_per_unit
    // at the driver), `out` the node voltage in the same unit, normalised by
    // the mid-band transfer g so a passband signal comes out at the level
    // it went in. The supply is shared by the channels.
    void tick(const double* in, double* out) noexcept SONORA_RT;

    // Small-signal figures, from the circuit (not real-time): the demagnetised
    // core's low-field inductance is measured, not assumed, by driving it.
    double mid_band_transfer() const noexcept { return g_; }
    double r_parallel() const noexcept { return rp_; }
    double rail_voltage() const noexcept { return rail_; }
    double idle_rail_voltage() const noexcept { return v_idle_; }
    double flux(std::uint32_t ch) const noexcept { return ch_[ch].psi; }
    double magnetising_current(std::uint32_t ch) const noexcept { return ch_[ch].i; }
    std::uint32_t max_iterations_seen() const noexcept { return max_iter_seen_; }

private:
    struct Core {
        double psi = 0.0, i = 0.0, v = 0.0;  // flux linkage, magnetising current, node voltage
        double m = 0.0, m_irr = 0.0;
        double slope = 0.0;                  // last secant slope, to warm-start
    };
    struct Eval {
        double psi, m, m_irr, dpsi;
    };
    Eval eval(const Core& s, double i) const noexcept SONORA_RT;
    double step(Core& s, double vs) noexcept SONORA_RT;

    TransformerCircuit ckt_{};
    CoreParams p_{};
    std::uint32_t channels_ = 1;
    double t_ = 0.0;          // sample period
    double g_ = 1.0, rp_ = 1.0;
    double v_idle_ = 0.0;     // reservoir voltage at idle
    double sag_ = 1.0;
    double rsup_ = 0.0;       // r_supply * sag
    double vres_ = 0.0;       // the reservoir's voltage
    double rail_ = 0.0;
    double v_open_ = 0.0;     // unloaded supply voltage, for the current sag
    Core ch_[kMaxChannels];
    std::uint32_t max_iter_seen_ = 0;
};

}  // namespace sonora::dsp
