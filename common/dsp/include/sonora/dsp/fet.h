// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gain-reduction stage and side chain of a 1960s FET peak limiter,
// modelled from the published circuit and from device physics (ADR-0011
// §3.3; stage N2, slice 6b). Never fitted to, or compared with, any other
// product's output.
//
// Sources (full citations, and what each one settles, in
// brain/research/native-fet-n2.md):
// - [M] The reissue manual of the original unit (2009), "Circuit Details":
//   Fig. 3 (gain reduction stage: R5 27k into the FET Q1 as the bottom leg
//   of a voltage divider; the gate fed through R7 2.2M || C6 200p from the
//   control voltage on C22 0.22u, and through R6 2.2M || C3 200p and C9
//   0.22u from the preamplifier's output via R30 6.8k and R86 100, the LN
//   circuit's trim); Fig. 5 (the side chain: from the preamplifier's
//   output through R6 1k into the ratio string R19 68k, R20 56k, R21 56k,
//   R22 47k, the switches' A sections choosing the tap; the phase inverter
//   Q7 (R38 47k, R39 4.7k) and emitter followers into the full-wave
//   rectifier CR2, CR3; the attack network R54 470 || C27 0.022u and R55
//   25k into C22; the release network R56 5M and R57 270k; the switches'
//   B sections choosing a tap of the bias string R76 150, R75 470, R73
//   560, R68 1.5k, R64 1.5k fed from -10 V through R35 10k); the text: "a
//   feedback style compressor", the bias "applied to the diodes by the
//   Compression Ratio pushbutton switches ... controls the threshold",
//   "higher ratios also set the threshold higher", the "Q" bias
//   calibration ("adjust R59 CW until the output decreases by 1.0 dB"),
//   attack 20 to 800 us, release 50 ms to 1.1 s, the all-button mode, and
//   the stereo adapter doubling the fastest attack.
// - [SH] H. Shichman and D. A. Hodges, "Modeling and simulation of
//   insulated-gate field-effect transistor switching circuits", IEEE JSSC
//   3(3), 1968: the square-law channel, with channel-length modulation, as
//   SPICE's JFET level 1 implements it for either sign of Vds.
// - [J] The 2N5457's published SPICE parameters (National Semiconductor's
//   model, 1988): BETA 1.125 mA/V^2, VTO -1.372 V, LAMBDA 2.3 mV^-1; IDSS
//   2.1 mA and VGS(off) 1.37 V, inside the datasheet's 1-5 mA and
//   0.5-6 V.
// - [D] The 1N4148 SPICE model as published (IS 2.52 nA, N 1.752): the
//   rectifier diodes, whose type [M] does not give.
// - [Y] D. T. Yeh, J. S. Abel and J. O. Smith, "Automated physical modeling
//   of nonlinear audio circuits for real-time audio effects, Part I", IEEE
//   TASLP 18(4), 2010: the method, the circuit's linear part solved
//   symbolically and Newton's method on its nonlinear ports only.
//
// The circuit:
//
//   vin --R5 27k--+-- drain (to the preamplifier, gain Gp)
//                 |
//                 Q1, gate = (Vc + Vq) / 2 + vd / 2
//
// - The LN circuit. The gate sits at the middle of two equal arms: R7 from
//   C22 (the control voltage Vc), R6 from C9, which carries the
//   preamplifier's output through R30 and the trim R86. R86 is trimmed so
//   the gate gets half the drain's audio (the trim's null). Then [SH]'s
//   ohmic current, beta vds (2 (vgs - vto) - vds), loses its vds^2 term:
//   the channel is a linear resistor, 1 / (2 beta (Vg0 - vto)), for either
//   sign of vds, and the distortion left is channel-length modulation and
//   the saturated region at light reduction. That is why the stage is
//   linear enough to be a gain cell [M].
// - C9 blocks DC, so for the slow control voltage the gate follows Vc
//   fully only after (R6 + R7) C9, about 1 s: a step of Vc reaches the
//   gate half at once and half over a second. Vq is C9's charge.
// - Gp: R86 at its 100 ohm limit nulls with R30 6.8k at a preamplifier
//   gain of 69 from the drain, the smallest at which the trim can null;
//   taken as that gain (the research note says why).
// - The side chain: s = A k_r Gp vd, k_r the ratio string's tap, A the
//   inverter's gain R38 / R39, the swing bounded by the followers' supply.
//   The two rectifier anodes sit at Vb_r + s and Vb_r - s, Vb_r a tap of
//   the bias string below the rest point, and charge C22 through R54 ||
//   C27 and R55 (attack); R57 + R56 (release) return C22 to the rest point.
// - The rest point: the "Q" bias calibration of [M], held: at rest the
//   divider loses 1.0 dB. The rest voltage is solved so that holds with
//   the rectifiers' leakage at the present ratio and release.
//
// Solving it, per sample, at the sample's instant: the capacitors are
// trapezoidal and the linear network is eliminated symbolically [Y], which
// leaves two unknowns per channel, the drain voltage and the rectifier's
// junction voltage, and one Newton solve of the two together (four for a
// linked pair), with SPICE's junction step limit and the drain bracketed.
// Its cost is flat in the drive: the input is bounded at R5
// (input_limit_v), and the drain starts from whichever law, ohmic or
// saturated, holds there. Solving them together matters: taking the drain from the gate of the
// sample before puts a sample of delay in the loop, and against ngspice
// that read 0.04 dB late at 192 kHz on a 1 kHz attack and 5.5 dB on the
// fastest attack's 10 kHz burst. Against ngspice on the same reduced
// circuit (the gate, side chain, bias taps and rest point behavioural
// sources) with its own JFET and diode models: plugins/native/fet/tests/
// spice/.
//
// Which values come from where. Every resistor and capacitor is [M]'s;
// the devices are [J] and [D]. Assumed or fitted, because [M] gives no
// figure: Gp and A (from the values, above), the bias string's drive (the
// -10 V supply), the followers' swing, the gate's forward limit and the
// full-scale level (FetCircuit says each). The research note lists them,
// with what would replace each.
//
// Real time: prepare() allocates nothing; every other call is bounded
// arithmetic, noexcept SONORA_RT. The Newton loops have fixed caps.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::dsp {

// [SH] as SPICE's NJF level 1, with [J]'s values.
struct JfetParams {
    double beta = 1.125e-3;  // A/V^2
    double vto = -1.372;     // V
    double lambda = 2.3e-3;  // 1/V
};

// [D]: the rectifier diodes.
struct DiodeParams {
    double is = 2.52e-9;  // A
    double n = 1.752;
    // kT/q at ngspice's default 27 C (300.15 K), so the SPICE reference
    // and the model use the same junction law.
    double vt = 8.617333262e-5 * 300.15;
};

// [M] Fig. 3 and Fig. 5, ohms, farads, volts; the assumed values marked.
struct FetCircuit {
    double r5 = 27e3;
    // The gate's two arms and C9; C3 and C6 equal and the arms equal, so
    // the divider is flat at audio and they are left out.
    double r6 = 2.2e6, r7 = 2.2e6, c9 = 0.22e-6;
    double c22 = 0.22e-6;
    double r54 = 470.0, c27 = 0.022e-6, r55_max = 25e3;  // attack
    double r57 = 270e3, r56_max = 5e6;                   // release
    // Ratio string from the preamplifier's output (side-chain R6 1k).
    double rs_in = 1e3, r19 = 68e3, r20 = 56e3, r21 = 56e3, r22 = 47e3;
    // Bias string, from the rest node: R76, then the taps of 4:1, 8:1,
    // 12:1, 20:1 between R75, R73, R68, and R64 to its feed through R35.
    double r76 = 150.0, r75 = 470.0, r73 = 560.0, r68 = 1.5e3, r64 = 1.5e3, r35 = 10e3;
    // Gp: the preamplifier's gain from the drain at R86's null (above).
    double preamp_gain = 69.0;
    // [M]: the "Q" bias calibration, the divider's loss at rest.
    double rest_loss_db = 1.0;
    // A: the inverter Q7's gain, R38 / R39 = 47k / 4.7k.
    double sc_gain = 10.0;
    // Assumed: the bias string fed from the -10 V supply through R35 (the
    // figure draws R35 to it); the followers' swing on the side chain's
    // supply; the gate's forward limit (a silicon junction fed through
    // R6 || R7, 1.1 Meg).
    double bias_drive_v = 10.0;
    double sc_swing_v = 12.0;
    double gate_max_v = 0.5;
    // Assumed: the most the input stage delivers at R5, |vin| bounded
    // there. No voltage in the unit exceeds its supply, so 30 V is a
    // physical ceiling, not a figure of [M]'s; it is ten times the
    // hottest the suite's checks drive (3.2 V: +24 dBFS at Input +24 dB),
    // so nothing inside that range changes. Beyond it, a processor's
    // input up to its 1e6 FS ceiling (2e5 V here at Input +24) would only
    // grow the Newton solve's cost (RT audit of #74, B1), and far beyond,
    // overflow it.
    double input_limit_v = 30.0;
    // Volts at R5 for a full-scale sample at the input control's 0 dB:
    // our alignment, set so 4:1 reduces by 1 dB at -20 dBFS at the
    // defaults: [M]'s getting-started level, 0 VU (+4 dBu, -14 dBFS under
    // EBU R68), then a few dB of reduction.
    double full_scale_v = 0.0126;
};

// The ratio switch, as positions: 0..3 the buttons 4, 8, 12, 20; 4 all
// four in. Fractional positions interpolate the side-chain tap and the
// bias (the processor smooths a change over 20 ms; [M]'s later revision
// added a resistor across the switch against pops for the same reason).
constexpr int kFetAllButtons = 4;

class FetChannel {
public:
    void prepare(double sample_rate, const FetCircuit& c = {}, const JfetParams& j = {},
                 const DiodeParams& d = {}) noexcept;
    void reset() noexcept SONORA_RT;

    // Controls; recompute the rest point.
    void set_ratio(double position) noexcept SONORA_RT;
    // R55 and R56 in ohms (0 = fastest).
    void set_attack_ohms(double r55) noexcept SONORA_RT;
    void set_release_ohms(double r56) noexcept SONORA_RT;
    // The three at once, one rest-point solve: what a processor calls at
    // its control rate.
    void set_controls(double position, double r55, double r56) noexcept SONORA_RT;

    // One sample. `loop_vin`, volts at R5, is the input whose drain drives
    // the side chain: the input itself, or a key through a second divider
    // on the same gate. The drain, the side chain and the capacitors are
    // solved together at the sample's instant; loop_drain() is the result.
    void step(double loop_vin) noexcept SONORA_RT;
    // A linked pair: the two C22 nodes joined through g_link siemens (the
    // stereo adapter, [M]); both channels solved together.
    static void step_pair(FetChannel& a, FetChannel& b, double vin_a, double vin_b, double g_link) noexcept SONORA_RT;
    double loop_drain() const noexcept { return vd_; }
    // The drain for vin at the present gate: the audio when the side chain
    // follows a key.
    double drain(double vin) const noexcept SONORA_RT;
    // step() and its drain: one unlinked, unkeyed sample. What the SPICE
    // comparison runs.
    double tick(double vin) noexcept SONORA_RT {
        step(vin);
        return vd_;
    }
    // Newton iterations the last step took (the loops' cap is 60; 0 when
    // the rectifier was off and the step explicit): for the tests that
    // hold the cost flat in the drive.
    int last_iterations() const noexcept { return iterations_; }
    // Steps whose Newton loop reached the cap without converging (they
    // commit the last point), or whose point was not finite (the channels
    // solved together reset to rest instead): zero in every test, and the
    // suite holds it. A processor reports a rise as a bounded block. Read
    // on the audio thread only.
    std::uint64_t cap_hits() const noexcept { return cap_hits_; }
    // For the tests: Newton iterates to round-off (corrections under
    // 1e-11 V on the junction and 1e-12 of the drive) instead of stopping
    // on the corrections' size (1e-4 V, 1e-5), so the stop rule's own
    // error can be measured against the root (test_fet). Read from the
    // first channel of a pair.
    void set_reference_stop(bool on) noexcept { reference_stop_ = on; }

    // The divider's gain at rest, for normalising: 10^(-rest_loss/20).
    double rest_gain() const noexcept { return rest_gain_; }
    // The gain reduction against rest, dB, from the gate's state (small
    // signal: the ohmic channel).
    double gain_reduction_db() const noexcept;
    double control_v() const noexcept { return vc_; }
    double c9_v() const noexcept { return vq_; }
    double rest_v() const noexcept { return v_rest_; }
    double bias_v() const noexcept { return vb_; }
    double tap() const noexcept { return k_; }
    const FetCircuit& circuit() const noexcept { return c_; }
    const JfetParams& jfet() const noexcept { return j_; }
    const DiodeParams& diode() const noexcept { return d_; }

    // The ratio string's tap and the bias string's offset below the rest
    // node at a switch position (fractional positions interpolate).
    static void ratio_network(const FetCircuit& c, double position, double& tap, double& offset_v) noexcept SONORA_RT;
    // [SH] symmetric, with the LN gate (vg0 + vds / 2): the drain current
    // for a drain voltage; its slopes in vds and in vg0.
    static double channel_current(const JfetParams& j, double vg0, double vds, double* slope = nullptr,
                                  double* slope_gate = nullptr) noexcept SONORA_RT;
    // The rest voltage for which the quiet equilibrium of C22 (rectifier
    // leakage against the release resistor) is `target`.
    static double rest_for(const DiodeParams& d, double target, double offset_v, double ra, double rr) noexcept SONORA_RT;

private:
    struct Linear;
    void update() noexcept SONORA_RT;
    void linear_step(Linear& l) const noexcept SONORA_RT;
    // The diode pair's current at junction voltage u (the higher anode's)
    // and drive |s|; its slopes in u and in |s|.
    double pair_current(double u, double s_abs, double* du, double* ds) const noexcept SONORA_RT;
    void commit(const Linear& l, double i_det, double vc_new, double link_i, double vd) noexcept SONORA_RT;
    template <int N>
    static void solve(FetChannel* const* ch, const double* vin, double g_link) noexcept SONORA_RT;

    FetCircuit c_;
    JfetParams j_;
    DiodeParams d_;
    double dt_ = 1.0 / 48000.0;
    double nvt_ = 0.045, vcrit_ = 0.0;
    double ratio_ = 0.0, r55_ = 0.0, r56_ = 0.0;
    double k_ = 0.0, offset_ = 0.0, gr_ = 0.0, gq_ = 0.0, gz_ = 0.0;
    // Per-step constants, from update(): the trapezoidal capacitances
    // (2C / dt) and the reciprocals the step would otherwise divide by.
    double k9_ = 0.0, inv_kk9_ = 0.0, beta_q_ = 0.0, k22_ = 0.0, k_c22_ = 0.0, q_ = 0.0, k27_ = 0.0, inv_gz_ = 0.0;
    double inv_nvt_ = 0.0, g0_ = 0.0, r_att_ = 0.0;
    double v_target_ = 0.0, v_rest_ = 0.0, vb_ = 0.0, rest_gain_ = 1.0, sc_gain_ = 0.0;
    // State: C22 (vc_), C9 (vq_) and C27 (vz_), each with its last
    // capacitor current (trapezoidal: f22_, f9_, i27_), the last junction
    // voltage (Newton's start) and drain.
    // Denormals: vc_ and vq_ rest at the rest voltage (about -1.37 V), u_
    // at the bias below rest, i_det_ at the rectifiers' leakage (about
    // -5 nA) and vz_ at that through R54: none decays toward zero. The
    // others do, after the signal stops: i27_ geometrically (by 0.6 or less
    // a sample) and f22_, f9_ with C22's and C9's time constants, so they
    // pass through the subnormal range, i27_ within some 1500 samples and
    // f9_ after minutes of silence; vd_ is exactly zero on zero input. The
    // processor's DenormalGuard (arch/) flushes them, as ProcessorBase
    // holds one; on the generic arch, which has no flush, they run slow
    // rather than wrong (the tube's and opto's caveat).
    double vc_ = 0.0, vq_ = 0.0, f22_ = 0.0, f9_ = 0.0, u_ = 0.0, i_det_ = 0.0, vz_ = 0.0, i27_ = 0.0;
    double vd_ = 0.0;
    // The last solution's drive and rectifier slope: the next start.
    double s_last_ = 0.0, iu_last_ = 0.0;
    // The gate base before the last step: the start extrapolates it.
    double vg_last_ = 0.0;
    // As the first of a linked pair: the joined network's inverse, and the
    // conductance and constants it was made for (-1: not yet made).
    double link_q_[3] = {0.0, 0.0, 0.0};
    double link_g_ = -1.0, link_ka_ = -1.0, link_kb_ = -1.0;
    int iterations_ = 0;
    std::uint64_t cap_hits_ = 0;
    bool reference_stop_ = false;
};

}  // namespace sonora::dsp
