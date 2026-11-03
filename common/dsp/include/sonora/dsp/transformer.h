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
// integrated with the trapezoidal rule. Each step solves one scalar
// equation for i. Its residual is strictly increasing in i (psi(i) does
// not decrease, and -Rp i adds a positive slope), so the root is bracketed
// from one evaluation and a safeguarded Newton converges; the bound is
// reported (max_iterations_seen) and held by a test.
//
// The core may step slower than the driver (`substeps`, prepare): the
// driver's clip runs on every sample, the core once per `substeps`, with
// its current taken linear across the step and the driver's flux summed
// sample by sample. The magnetising current is the integral of the node
// voltage through the core's inductance, a low-passed signal (the
// low-field corner is near 40 Hz), so the tube drive runs the core at its
// base rate and the clip oversampled: the solve is the stage's cost, and
// it then costs the same at every quality (brain/research/native-tube-n2.md,
// "Output stage at class B").
//
// Why it is "LF first": the flux is the integral of the voltage, so for
// a given level it grows as 1 / f. The core saturates on bass first, the
// hysteresis loop is widest there, and the magnetising current (the
// distortion) rises as the frequency falls. The mid-band is nearly
// linear. Measured in common/dsp/tests/test_transformer.cpp.
//
// Real-time safe: no allocation after prepare(), bounded iterations, finite
// output for any input (non-finite input is read as 0).
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
    // Samples per core step, at most: the tube drive's highest oversampling.
    static constexpr std::uint32_t kMaxSubsteps = 8;
    // The solve's default convergence, as the output error the last Newton
    // step may stand for (signal units, 1.0 = full scale). That step is
    // taken along the evaluated slope, which removes most of it: against a
    // solve converged to -200 dB the output differs by -94 dB of its peak
    // at worst (programme 12 dB into the rail, 48 kHz), 25 dB under what
    // stepping the core at 48 rather than 96 kHz changes (-55 to -68 dB),
    // for 1.09 evaluations per step at most where 1e-5 took 1.67
    // (test_dsp_transformer, "solve").
    static constexpr double kDefaultSolveTol = 1e-4;
    // The Langevin function's table: segments of 1/32 over [0, 20].
    static constexpr std::uint32_t kLangevinSegments = 640;

    // Not real-time. `sample_rate` is the rate of the samples `process` is
    // given; the core steps once per `substeps` of them (1..kMaxSubsteps).
    // Returns false for a non-positive rate, a channel count outside
    // 1..kMaxChannels, a substep count outside its range or a circuit that
    // is not physical.
    bool prepare(double sample_rate, std::uint32_t channels, const TransformerCircuit& c = {},
                 const CoreParams& p = {}, std::uint32_t substeps = 1);
    // Rest: core demagnetised, reservoir at its idle voltage.
    void reset() noexcept SONORA_RT;
    // Not real-time: for measurement (the solve test converges it to -200 dB).
    void set_solve_tolerance(double output_tol) noexcept;
    // Not real-time: for measurement. Two channels run as the two lanes of
    // one solve (src/arch/<arch>/transformer_pair.cpp) unless this is
    // false; the lanes give the bits the one-channel solve gives, which the
    // suite holds.
    void set_pair_solve(bool on) noexcept { pair_ = on; }

    // 0 = a stiff supply (the rail never moves), 1 = the full source
    // resistance. May change every step; the reservoir state is continuous.
    void set_sag(double amount) noexcept SONORA_RT;

    // One core step: `substeps` samples on every channel, in[c][j] and
    // out[c][j]. `in` is the signal (1.0 = volts_per_unit at the driver),
    // `out` the node voltage in the same unit, normalised by the mid-band
    // transfer g so a passband signal comes out at the level it went in.
    // The supply is shared by the channels.
    void process(const double* const* in, double* const* out) noexcept SONORA_RT;
    // `steps` core steps in one call (steps * substeps samples per channel):
    // what process() does that many times, to the bit, in less time. With
    // `sag` not null, set_sag(sag[t / steps_per_sag]) runs before step t.
    void process_block(const double* const* in, double* const* out, std::uint32_t steps, const double* sag,
                       std::uint32_t steps_per_sag) noexcept SONORA_RT;
    // One sample on every channel, interleaved, for a stage prepared with
    // one substep (otherwise the output is 0).
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
    // Core evaluations per step since reset(): the solve's cost in a figure
    // no machine moves (the suite holds it).
    double mean_iterations() const noexcept {
        return steps_ == 0 ? 0.0 : static_cast<double>(evals_) / static_cast<double>(steps_);
    }

private:
    struct Core {
        double psi = 0.0, i = 0.0;  // flux linkage, magnetising current
        double m = 0.0, m_irr = 0.0;
        double vs_last = 0.0;       // the driver's last sample, for the trapezoid
        double inv_slope = 0.0;     // 1 / (dF/di) at the last step, to warm-start
    };
    // The core's step for V::kLanes channels at once (src/transformer_solve.h):
    // V is one double (Lane1, there) or a pair (the arch files).
    struct Lane1;
    template <class V>
    struct EvalV;
    // The core's state in lane layout, held in registers across a block.
    template <class V>
    struct StateV;
    // The core's constants in lane form, made once per block into a local:
    // already broadcast, and out of reach of the output stores (doubles),
    // which would otherwise force a reload of every member each step.
    template <class V>
    struct CoefV;
    template <class V>
    CoefV<V> coef() const noexcept SONORA_RT;
    template <class V>
    static EvalV<V> eval_lanes(const CoefV<V>& k, V si, V sm, V smirr, V i) noexcept SONORA_RT;
    // The safeguarded Newton loop, for a step the first evaluation leaves
    // more than di_tol_ from converged; one channel.
    void refine(const Core& s, double rhs, double& i, EvalV<Lane1>& e, double& f, double& inv, double& di,
                std::uint32_t& n) const noexcept SONORA_RT;
    template <class V>
    void process_lanes(const double* const* in, double* const* out, std::uint32_t steps, const double* sag,
                       std::uint32_t steps_per_sag) noexcept SONORA_RT;
    friend void transformer_process_pair(OutputStage& st, const double* const* in, double* const* out,
                                         std::uint32_t steps, const double* sag,
                                         std::uint32_t steps_per_sag) noexcept SONORA_RT;

    TransformerCircuit ckt_{};
    CoreParams p_{};
    std::uint32_t channels_ = 1;
    std::uint32_t substeps_ = 1;
    double inv_substeps_ = 1.0;
    double t_ = 0.0;          // sample period
    double t_step_ = 0.0;     // the core's step: substeps * t_
    double half_ = 0.0;       // t_step_ Rp / 2, the trapezoid's
    double di_tol_ = 0.0;     // the solve's convergence step, amperes
    double inv_slope_rest_ = 0.0;
    double g_ = 1.0, rp_ = 1.0;
    double inv_a_ = 1.0, inv_rs_ = 1.0, inv_out_ = 1.0;
    double sup_a_ = 0.0, sup_open_ = 0.0, sup_inv_den_ = 1.0;  // set_sag's, for the reservoir
    double v_idle_ = 0.0;     // reservoir voltage at idle
    double sag_ = 1.0;
    double rsup_ = 0.0;       // r_supply * sag
    double vres_ = 0.0;       // the reservoir's voltage
    double rail_ = 0.0;
    // The rail the driver clips to: rail_ one core step earlier. The extra
    // step of lag (20.8 us at eco's 48 kHz core rate, 10.4 us at 96 kHz,
    // under 4e-4 of the reservoir's 60 ms) takes the supply off the step's
    // chain of dependent operations: the clip of step t no longer waits
    // for the current drawn in step t - 1 (brain/research/native-tube-n2.md,
    // "Output stage at class B", the rerun).
    double rail_drv_ = 0.0;
    double v_open_ = 0.0;     // unloaded supply voltage, for the current sag
    Core ch_[kMaxChannels];
    // L and L' at each node, filled by prepare: a pair of doubles so one
    // load reads a node (src/transformer_solve.h).
    double lan_[kLangevinSegments + 1][2] = {};
    std::uint32_t max_iter_seen_ = 0;
    std::uint64_t evals_ = 0, steps_ = 0;
    bool pair_ = true;
};

// Two channels of `st` in lockstep (src/arch/<arch>/transformer_pair.cpp):
// OutputStage::process_block's work.
void transformer_process_pair(OutputStage& st, const double* const* in, double* const* out, std::uint32_t steps,
                              const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT;

}  // namespace sonora::dsp
