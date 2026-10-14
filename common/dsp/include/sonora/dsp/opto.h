// SPDX-License-Identifier: GPL-3.0-or-later
//
// The electro-optical gain cell of a 1960s optical levelling amplifier,
// and the attenuator and side chain around it, modelled from the published
// circuit and from device physics (ADR-0011 §3.3; stage N2, slice 6).
// Never fitted to, or compared with, any other product's output.
//
// Sources (full citations, and what each one settles, in
// brain/research/native-opto-n2.md):
// - [M] The reissue manual of the original unit (rev. 1.3, 2000): the
//   input and gain-reduction schematic (its Fig. 5), the side-chain
//   schematic (Fig. 6), the description of the cell, and the published
//   specification: up to 40 dB of reduction; release "approximately 0.06
//   seconds for 50% release, 0.5 to 5 seconds for complete release
//   depending upon the amount of previous reduction"; "very fast attack";
//   the cell "releases quickly (40-80 milliseconds) to approximately half",
//   then "over as much as several seconds", slower after longer or
//   heavier reduction; a "feed-back style" side chain; "flat" side-chain
//   response at the factory setting of the pre-emphasis control.
// - [EL] The brightness law of AC powder electroluminescence,
//   B = B0 exp(-b / sqrt(V)): G. F. Alfrey and J. B. Taylor, Proc. Phys.
//   Soc. B 68 (1955) 775; P. Zalm, Philips Res. Rep. 11 (1956) 353-451.
// - [PC] Photoconductor kinetics, free carriers with linear and
//   bimolecular recombination exchanging with trap levels: A. Rose,
//   *Concepts in Photoconductivity and Allied Problems*, Interscience,
//   1963, ch. 3-4; R. H. Bube, *Photoconductivity of Solids*, Wiley, 1960,
//   ch. 9. Shallow traps that fill during illumination and empty slowly
//   afterwards are what make a CdS cell's decay two-stage and its length
//   depend on the illumination's history: the "memory" [M] describes.
//
// The circuit, values from [M] Fig. 5 and Fig. 6:
//
//   vin --R6 68k--+--R7 2.7k--+-- out (to the gain control, R1 100k)
//                 |           |
//                tap        cell (photoresistor) || R1 100k -- gnd
//
//   tap --C6 10n--+--R30 47k--R2 100k (peak reduction, wiper) -- voltage
//   amplifier -- R37 (0..1 Meg) || C12 1n --+-- R3 1 Meg -- gnd
//                                           |
//                                     driver grid -- driver -- EL panel
//
// - The side chain is taken at the junction of R6 and R7, the lead [M]
//   Fig. 5 draws "to gain reduction control". The limit/compress switch
//   is not drawn in [M]; it is modelled as published by builders of the
//   circuit: closed (compress) it shorts R7, so the tap sits across the
//   cell and the loop is pure feedback; open (limit), R7 is in series and
//   the tap partly sees the input, which raises the ratio. This reading
//   is Unverified in the research note until hardware settles it.
// - The side chain does not load the tap. As drawn, C6 into 147 k on the
//   R6/R7 junction would put a 2 dB shelf near 85 Hz into the audio path,
//   which [M]'s response (+-0.1 dB, 30 Hz to 15 kHz) rules out; so the tap
//   is taken as buffered. Unverified, like the switch.
// - C6 into R30 + R2 high-passes the side chain at 108 Hz. The voltage
//   amplifier and the driver are taken as linear gains (`amp_gain`), and
//   the driver's swing as a hard limit (`drive_limit_v`): a plate cannot
//   swing past its supply.
// - R37 and C12 into R3: the pre-emphasis. Flat at R37 = 0, the factory
//   setting; at 1 Meg, the side chain is 6 dB less sensitive below about
//   160 Hz than above 320 Hz, so high frequencies compress more [M].
// - The gain control (R1's wiper) follows the attenuator, so it does not
//   change the compression [M]; the processor applies it after the cell.
//
// The cell (OptoCell): the EL panel's light follows [EL] on the
// instantaneous drive voltage (the panel emits on each half cycle, which
// a cycle average of [EL] on |v| represents; its phosphor persistence is
// left out, no published figure for this panel having been found). The
// photoconductor [PC], in units where n = 1 is a conductance of 1 / 400
// Ohm:
//   dn/dt  = k L - n / tau_r - B n^2 - sum_i c_i n (1 - m_i) + sum_i (N_i / tau_i) m_i
//   dm_i/dt = (c_i n (1 - m_i) - (N_i / tau_i) m_i) / N_i,   i = 1, 2
// n: free carriers; m_i: the occupancy of trap level i (density N_i,
// capture rate c_i, thermal emission time tau_i). Conductance
// Gdark + n / 400 Ohm.
//
// Which values come from where. Every circuit value is [M]'s. The cell's
// constants are not published for this cell: they are physical quantities
// set so the circuit meets [M]'s published behaviour, and each is held to
// it by the processor's suite (plugins/native/opto/tests/suite.cpp):
// - tau_r and the two trap levels: 50 % release in 40-80 ms and 90 %
//   release in 0.5-5 s, longer after longer and deeper reduction;
// - el_b = 55 V^1/2 is in the range of [EL]'s fits for ZnS:Cu panels;
//   el_gain and drive_limit_v: the deepest reduction is [M]'s 40 dB;
// - amp_gain: at full peak reduction, reduction starts near -40 dBFS.
// The research note lists them as fitted, with what would replace each
// (the owner's hardware; the CC BY 4.0 recordings of an original unit).
//
// Integration: one forward-Euler step per sample from the old state, the
// light taken as the midpoint value of its step and the conductance the
// next sample sees extrapolated the half step (OptoCell::tick says why).
// The generation is bounded by the panel's maximum, so the carriers are
// bounded and the step is stable and positive at any audio rate. Against
// ngspice's trapezoidal solution of the same equations (the SPICE
// reference, plugins/native/opto/tests/spice/): within 0.016 dB at four
// times 44.1 to 96 kHz, and 0.11 dB at the base rate in the first
// millisecond of a 30 dB attack. The loop takes the side chain from the
// cell's state at the start of the sample.
//
// Real time: prepare() allocates nothing; every other call is bounded
// arithmetic, noexcept SONORA_RT. The audio path is a time-varying
// resistive divider; the gain's movement with the light's ripple, times
// the signal, has components above Nyquist, so a processor runs this
// oversampled for its default quality (plugins/native/opto: measured).
#pragma once

#include "sonora/rt.h"

namespace sonora::dsp {

struct OptoCellParams {
    // [EL]: brightness exp(-el_b / sqrt(|v|)), v in volts at the panel.
    double el_b = 55.0;
    // Carrier generation per unit of light, 1/s (n units).
    double el_gain = 2600.0;
    // [PC]: linear recombination time, s; bimolecular coefficient, 1/s.
    double tau_r = 0.047;
    double bimolecular = 5.0;
    // Trap level 1 (shallow, fast) and 2 (deeper, slow): capture rate
    // (1/s), density (n units), thermal emission time (s).
    double trap1_capture = 4.0, trap1_density = 0.3, trap1_tau = 1.0;
    double trap2_capture = 1.0, trap2_density = 1.0, trap2_tau = 8.0;
    // Conductance per unit n, and in the dark, siemens.
    double unit_conductance = 1.0 / 400.0;
    double dark_conductance = 1.0 / 5.0e6;
};

class OptoCell {
public:
    void prepare(double sample_rate, const OptoCellParams& p = {}) noexcept;
    void reset() noexcept SONORA_RT;

    // [EL], relative units: 0 where it would underflow (|v| below
    // (el_b / 700)^2, 6 mV), so silence costs no exp and leaves no
    // subnormal. The cell's and the side chain's states are another
    // matter: OptoChannel's private members say how they are flushed.
    double light(double drive_v) const noexcept SONORA_RT;
    // One sample of the photoconductor under `generation` (k L, 1/s).
    void tick(double generation) noexcept SONORA_RT;
    // Light from the drive voltage and one sample.
    void drive(double drive_v) noexcept SONORA_RT { tick(p_.el_gain * light(drive_v)); }

    double conductance() const noexcept { return p_.dark_conductance + p_.unit_conductance * nx_; }
    // The free carriers at the next audio sample.
    double carriers() const noexcept { return nx_; }
    double trap1() const noexcept { return m1_; }
    double trap2() const noexcept { return m2_; }
    const OptoCellParams& params() const noexcept { return p_; }

    // The free carriers held by a constant generation once the traps are
    // in equilibrium with them: generation = n / tau_r + B n^2.
    static double steady_carriers(double generation, const OptoCellParams& p) noexcept;

private:
    OptoCellParams p_;
    double dt_ = 1.0 / 48000.0;
    double inv_tau_r_ = 0.0, e1_ = 0.0, e2_ = 0.0, k1_ = 0.0, k2_ = 0.0, inv_t1_ = 0.0, inv_t2_ = 0.0;
    double floor_v_ = 0.0;
    double n_ = 0.0, nx_ = 0.0, m1_ = 0.0, m2_ = 0.0;
};

// [M] Fig. 5 and Fig. 6, ohms, farads and volts.
struct OptoCircuit {
    double r6 = 68e3, r7 = 2.7e3, r1 = 100e3;
    double c6 = 10e-9, r30 = 47e3, r2 = 100e3;
    double r37_max = 1e6, c12 = 1e-9, r3 = 1e6;
    // Voltage gain of the side chain's amplifier and driver at full peak
    // reduction, and the driver's swing.
    double amp_gain = 310.0;
    double drive_limit_v = 142.0;
    // Volts at the attenuator for a full-scale sample: EBU R68 alignment
    // (0 dBu = -18 dBFS), so a full-scale sine is +18 dBu, 8.72 V peak.
    // The input transformer's ratio is folded into amp_gain.
    double full_scale_v = 8.7178;
};

// The peak reduction control's law: an audio-taper potentiometer, the
// wiper's share of the voltage across it at position p in [0, 1]:
// (100^p - 1) / 99, 0 at p = 0 (no compression [M]), -21 dB at mid travel.
double peak_reduction_share(double position) noexcept SONORA_RT;

// One channel of the circuit: attenuator, side chain and cell. A sample
// is three calls, so a stereo processor can link the two side chains
// between the second and the third ([M]: the link joins the drivers'
// grids, where R3 trims the balance):
//   attenuate(x) -> output gain, tap   (from the cell's present state)
//   sidechain(tap volts) -> grid volts (C6 high-pass, R2, amplifier, R37)
//   drive(grid volts)                  (swing limit, EL panel, one cell step)
class OptoChannel {
public:
    void prepare(double sample_rate, const OptoCircuit& c = {}, const OptoCellParams& p = {}) noexcept;
    void reset() noexcept SONORA_RT;

    // Controls; cheap enough per sample, recomputed only on a change.
    // peak_share: peak_reduction_share() of the knob. limit: 0 compress
    // (R7 shorted) to 1 limit (R7 in), continuous so a switch is smoothed.
    // r37: the pre-emphasis resistance, 0 (flat) to r37_max.
    void set_peak_share(double share) noexcept SONORA_RT;
    void set_limit(double limit) noexcept SONORA_RT;
    void set_r37(double ohms) noexcept SONORA_RT;

    // The attenuator's gain from vin to out, normalised so a dark cell is
    // unity in the present switch position; and the tap's share of vin.
    void attenuate(double& gain, double& tap_share) const noexcept SONORA_RT;
    // Side-chain linear network, one sample: tap volts in, grid volts out.
    double sidechain(double tap_v) noexcept SONORA_RT;
    // The driver's swing limit, the panel and one cell step.
    void drive(double grid_v) noexcept SONORA_RT;

    // x in full-scale units; returns the attenuated sample, unity gain
    // with the cell dark: the three calls for one unlinked, unkeyed
    // channel. What the SPICE comparison runs.
    double tick(double x) noexcept SONORA_RT;

    double gain_reduction_db() const noexcept;
    const OptoCell& cell() const noexcept { return cell_; }
    const OptoCircuit& circuit() const noexcept { return c_; }
    double full_scale_v() const noexcept { return c_.full_scale_v; }

    // The analog side-chain network's response at f: the C6 high-pass and
    // the R37/C12/R3 pre-emphasis, without R2's divider, the peak
    // reduction share and the amplifier (a flat gain, sidechain_gain()).
    // The target the discretisation is held to.
    static double sidechain_analog_mag(const OptoCircuit& c, double r37, double f_hz) noexcept;
    // R2 / (R30 + R2) x amp_gain x the peak share: sidechain()'s flat gain.
    double sidechain_gain() const noexcept { return sc_gain_; }
    // Attenuator gain, normalised to the dark cell's, and tap share, for a
    // cell conductance: the formulas attenuate() uses, for references.
    static void divider(const OptoCircuit& c, double limit, double cell_conductance, double dark_conductance,
                        double& gain, double& tap_share) noexcept;

private:
    void update_emphasis() noexcept SONORA_RT;

    OptoCircuit c_;
    OptoCell cell_;
    double fs_ = 48000.0;
    double peak_share_ = 0.0, limit_ = 0.0, r37_ = 0.0;
    double r7e_ = 0.0, inv_a0_ = 1.0;
    // First-order sections, bilinear, unwarped: their corners are at or
    // below 320 Hz, where the transform moves a frequency by under 0.02 %
    // at 44.1 kHz, and they shape the side chain only. The suite holds
    // the discretised response to the analog one (sidechain_analog_mag).
    // Denormals: unlike light(), these states have no floor. After digital
    // silence the high-pass state decays by its pole (0.985 at 44.1 kHz,
    // 0.9965 at 4x 48 kHz) and the pre-emphasis state with it, reaching the
    // subnormal range in about a second at every rate; the cell's trap 2
    // after about 1.5 hours. The caller's DenormalGuard (arch/) flushes
    // them, as ProcessorBase::process holds one; on the generic arch, which
    // has no flush, they run slow rather than wrong (the tube's caveat).
    double hp_b0_ = 1.0, hp_a1_ = 0.0, hp_x1_ = 0.0, hp_y1_ = 0.0;
    double em_b0_ = 1.0, em_b1_ = 0.0, em_a1_ = 0.0, em_x1_ = 0.0, em_y1_ = 0.0;
    double sc_gain_ = 0.0;  // R2 / (R30 + R2) x amp_gain x peak share
};

}  // namespace sonora::dsp
