# Opto, stage N2 slice 6: the optical levelling amplifier

- Date: 2026-10-14
- Author: dsp-engineer
- Scope: ADR-0011 §6 N2, slice 6 of `planning/BACKLOG.md`, first half:
  - the electro-optical cell, the attenuator and the side chain in
    `common/dsp` (`opto.h`);
  - its SPICE reference (`plugins/native/measure`, `spice_opto.h`);
  - the processor on it (`plugins/native/opto`, "Sonora Opto"), with its
    LV2 and VST3 bundles, stereo and mono, with the key input.

  The FET peak limiter is the second half of slice 6, deferred to its own
  PR (BACKLOG). The original amplifier's tubes and transformers are a
  follow-up (below).
- Where: the `sonora-plugins` image (Debian trixie, GCC 14, ngspice 44.2)
  on the owner's laptop (11th Gen Intel Core i7-11800H, 2.30 GHz), under
  Docker Desktop on Windows, Release build. CPU figures are container
  figures, not appliance figures (ADR-0011 Amendment 1 item 4).

The original unit is the 1960s optical levelling amplifier the owner
named (the Teletronix LA-2A). This note names it because it is a research
note (`native-plugin-suite.md` §5); nothing in the product does: the
processor is "Sonora Opto", its category "optical levelling amplifier",
and the source code cites the manual without the name.

## Sources

| Ref | Source | Used for |
|---|---|---|
| [M] | Universal Audio, *Model LA-2A Leveling Amplifier* (reissue manual), part LA2A-M01, rev. 1.3, © 2000; copy read at https://www.barryrudolph.com/recall/manuals/uaaudiola2.pdf | Fig. 5 (input and gain-reduction circuit: R6 68 kΩ, R7 2.7 kΩ, gain control R1 100 kΩ, the cell's photoresistor shunting the R7/R1 junction, the EL panel through 0.0047 µF, the side chain taken "to gain reduction control"); Fig. 6 (side chain: C6 0.01 µF, R30 47 kΩ, peak reduction R2 100 kΩ, 12AX7 voltage amplifier, R37 1 MΩ "LIM RESP" across C12 0.001 µF into R3 1 MΩ at the 6AQ5 driver's grid); the cell's description; the specification page |
| [M] spec | Same, p. 1 | "Gain Reduction: up to 40 dB"; "Distortion: less than 0.35% total harmonics at +10 dBm"; "Response: +/- 0.1 dB, 30 cycles to 15 kilocycles"; "Very fast attack time"; "Release Time: approximately 0.06 seconds for 50% release, 0.5 to 5 seconds for complete release depending upon the amount of previous reduction" |
| [M] text | Same, pp. 9-11 | "releases quickly (40-80 milliseconds) to approximately half of its off resistance. The remainder of its release can take place over as much as several seconds"; "the release time is slower if the unit has either been in compression for a while, or the amount of compression is large"; "a feed-back style compressor"; R37 "factory set for a 'flat' side-chain response"; "Compress ... a low compression ratio. A higher compression ratio results when the switch is set to the Limit position"; the gain control "does not affect the compression" |
| [EL] | G. F. Alfrey and J. B. Taylor, "Electroluminescence in single crystals of zinc sulphide", Proc. Phys. Soc. B 68 (1955) 775; P. Zalm, "Electroluminescence of zinc sulphide type phosphors", Philips Res. Rep. 11 (1956) 353-451 | The brightness law of AC powder electroluminescence, B = B0 exp(-b / sqrt(V)) |
| [PC] | A. Rose, *Concepts in Photoconductivity and Allied Problems*, Interscience, 1963, ch. 3-4; R. H. Bube, *Photoconductivity of Solids*, Wiley, 1960, ch. 9 | Photoconductor kinetics: free carriers with linear and bimolecular recombination exchanging with trap levels; traps as the cause of the slow, history-dependent decay |
| [S] | Builders' accounts of the limit/compress switch (groupdiy.com threads "Please explain LA-2 limit compress switch" and "LA2A feedforward/feedback design?", read through search summaries; the forum refused direct fetches) | The switch shorts R7 in compress (the side chain then sits across the cell: pure feedback) and opens it in limit (the tap above R7 partly sees the input). **Unverified** |
| | EBU R68 | The level mapping: 0 dBu = -18 dBFS, so a full-scale sine is +18 dBu, 8.72 V peak |
| | ngspice 44.2 (BSD-3-Clause) | The SPICE reference |

The manual's text and drawings are copyrighted: the circuit values are
facts re-entered as our own netlist (`spice_opto.cpp`), and no drawing or
text is redistributed. No third-party code, no product's or plugin's
output used as a reference or fitting target, no preset copied (ADR-0004,
ADR-0011 §3.3, §5). The SignalTrain LA-2A recordings (Zenodo 3824876,
CC BY 4.0, 20.7 GB) were not used in this slice (below).

## The model

```
vin --R6 68k--+--R7 2.7k (shorted in compress)--+-- out -> gain -> mix
              |                                 |
             tap                      photoresistor || R1 100k
              |
   C6 10n -> R30 47k + R2 100k (peak reduction, audio taper wiper)
   -> amplifier (gain 310) -> R37 0..1 Meg || C12 1n into R3 1 Meg
   -> [stereo: grids joined] -> driver, swing +-142 V -> EL panel -> cell
```

**From [M], as published:** every resistor and capacitor above; the
feedback topology; the side chain at the R6/R7 junction (Fig. 5's lead);
pre-emphasis flat at R37 = 0; the gain control after the attenuator; the
stereo link joining the drivers (the R3 "stereo adj" calibration).

**From the literature:** the EL law [EL] applied to the instantaneous
drive (the panel emits on each half cycle); the photoconductor [PC]:

```
dn/dt   = k L - n / tau_r - B n^2 - sum_i c_i n (1 - m_i) + sum_i (N_i / tau_i) m_i
dm_i/dt = (c_i n (1 - m_i) - (N_i / tau_i) m_i) / N_i,      i = 1, 2
G = 1 / 5 MOhm + n / 400 Ohm
```

**Fitted, because no published figure exists for this cell** (each is a
physical quantity set so the circuit meets [M]'s published behaviour, and
the suite holds that behaviour, not the constant):

| Constant | Value | Set so that | Would be replaced by |
|---|---|---|---|
| tau_r, B | 47 ms, 5 /s | 50 % release in 40-80 ms | the cell's own decay, measured |
| trap 1: c, N, tau | 4 /s, 0.3, 1 s | 90 % release 0.5-5 s, lengthening with time held | same |
| trap 2: c, N, tau | 1 /s, 1.0, 8 s | the memory over 5-20 s holds | same |
| el_b | 55 V^1/2 | in the range of [EL]'s fits for ZnS:Cu powder panels | the panel's brightness against voltage |
| el_gain, drive limit | 2600 /s, 142 V | the deepest reduction is [M]'s 40 dB (39.97 dB measured) | the cell's resistance at full light |
| amplifier gain | 310 | at full peak reduction, reduction starts near -40 dBFS | the side chain's measured gain |
| dark conductance | 1 / 5 MΩ | a dark CdS cell, MΩ (no effect in the band of interest) | the cell's dark resistance |
| limit share | R7 = 2.7 kΩ in limit | [S] | the switch, traced on hardware |

**Two readings that are ours, and Unverified:**
- The side chain does not load the tap. As drawn, C6 into 147 kΩ on the
  R6/R7 junction would put a 2 dB shelf near 85 Hz into the audio path,
  which [M]'s flat response (±0.1 dB, 30 Hz-15 kHz) rules out. The model
  takes the tap as buffered; the SPICE netlist says the same (`EBUF`).
- The limit/compress switch as [S] describes it. With it, compress is a
  pure feedback loop with a ratio of 2.4:1 to 4:1 ("a low compression
  ratio" [M]) and limit rises to 18.6:1 at 15 dB of reduction and past
  infinity beyond (the output falls as the input rises): "a higher
  compression ratio" [M].

**The stereo link** joins the two drivers' grids, so each sees the mean of
the two side chains. Anti-phase content cancels in the mean and is not
compressed when linked: a property of joining the grids, which the suite
measures (`key_and_link`, `hot_input`).

## The processor

"Sonora Opto" (`org.sonora.opto`, category Dynamics), stereo and mono,
with a key input. Parameters, append-only: peak reduction (0-100, the
audio-taper control; default 70, a few dB of levelling on material
peaking near -12 dBFS), gain (-12 to +40 dB after the cell; 0 dB is unity
with no reduction, the original amplifier's 40 dB normalised out), mode
(compress, limit; R7 moves in over 20 ms rather than step), side-chain
emphasis (R37, 0-100 %), stereo link, mix (ours, for parallel
compression), external key, quality (eco 1x with no latency, standard 4x
by default, high 8x; reconfigures). Attack and release are the cell's,
not parameters.

## Integration

One step per sample of the rate the cell runs at (the base rate in eco,
4x in standard, 8x in high):
- the light of sample k is the midpoint value of the step from
  t_k - dt/2 to t_k + dt/2, and the conductance the next sample sees is
  extrapolated the half step (bounded to [n/2, 3n/2]);
- every state one forward-Euler step from the old state, with the
  panel's generation bounded by its maximum (the light is at most 1), so
  the carriers are bounded (about 22) and the fastest rate is a few
  thousandths of a step at 44.1 kHz; no division in the cell;
- the side chain reads the tap from the state at the start of the sample.

Against ngspice on the same equations, the first scheme tried (backward
Euler, light at the step's start) lagged half a sample: 0.13 dB at 48 kHz
and 0.26 dB on the +12 dBFS case in the first millisecond of a hard
attack. The midpoint and the half-step extrapolation cut that tenfold
(table below); the explicit step changed it by under 0.003 dB and took the
cell's three divisions out of the per-sample chain.

## Results (`native_opto`, `dsp_opto`, `spice_opto_*`, `tools/build-plugins.sh`)

### Against ngspice (plot: `docs/images/dsp/opto-vs-spice.png`)

Gain reduction through six tone bursts (quiet -60 dBFS, a step up, back to
quiet; 0.25 ms grid for 40 ms after each step), max |model - ngspice|:

| Case | eco 44.1 / 48 kHz | standard (4x): 176.4 / 192 / 384 kHz, held to 0.05 dB |
|---|---|---|
| compress, peak 100, 1 kHz, -20 dBFS (7.2 dB) | 0.0053 / 0.0048 | 0.0005 / 0.0005 / 0.0004 |
| compress, peak 100, 1 kHz, -6 dBFS (17.6 dB) | 0.021 / 0.020 | 0.0012 / 0.0014 / 0.0006 |
| limit, peak 100, 1 kHz, -20 dBFS (7.9 dB) | 0.0055 / 0.0050 | 0.0005 / 0.0005 / 0.0004 |
| compress, peak 60, 100 Hz, 0 dBFS (7.4 dB) | 0.0012 / 0.0012 | 0.0010 / 0.0010 / 0.0010 |
| compress, emphasis 100, 5 kHz, -20 dBFS (7.2 dB) | 0.033 / 0.026 | 0.0004 / 0.0004 / 0.0004 |
| compress, peak 100, 1 kHz, +12 dBFS (30.8 dB, driver limit) | 0.10 / 0.11 | 0.014 / 0.016 / 0.0033 |

The eco figures are reported: the +12 dBFS case's 0.1 dB is the first
millisecond of an attack of 30 dB. The eco processor equals the circuit
it wraps bit for bit (`max |difference| 0`). The references regenerate in
22 s; each `spice_opto_*` ctest re-runs one case and holds the netlist
byte for byte and the trajectory to 1e-5 dB.

### Static curve and ratio (plot: `docs/images/dsp/opto-static-curves.png`)

1 kHz sines from -50 to +6 dBFS, 30 s each, the output RMS over the last
0.5 s against the model's steady state solved from the equations (the
cycle-averaged light of the side chain driven by the tap, which depends on
the carriers; bisection): worst 0.0014 dB over six settings (compress at
peak reduction 50, 75, 100; limit at 75, 100; compress at 100 with full
emphasis), bar 0.1 dB.

| Reduction | 3 dB | 6 dB | 10 dB | 15 dB |
|---|---|---|---|---|
| compress ratio | 2.41:1 | 3.19:1 | 3.74:1 | 3.98:1 |
| limit ratio | 2.61:1 | 3.98:1 | 6.37:1 | 18.6:1 |

The ratio does not depend on the peak reduction (it moves the threshold
only), as [M] says of the control.

### Release and memory (plot: `docs/images/dsp/opto-release-attack.png`)

Compress, peak reduction 100, the input held at the level for the stated
steady reduction, then a -60 dBFS pilot; the reduction read sample by
sample as |input / output|, eco at 48 kHz:

| Held | Reduction at the drop | 50 % release | 90 % release |
|---|---|---|---|
| 1 s at 10 dB | 9.86 dB | 47.1 ms | 0.68 s |
| 5 s at 10 dB | 9.97 dB | 53.3 ms | 1.36 s |
| 20 s at 10 dB | 10.00 dB | 55.6 ms | 1.80 s |
| 5 s at 5 dB | 4.97 dB | 43.6 ms | 1.08 s |
| 5 s at 20 dB | 19.98 dB | 74.7 ms | 1.85 s |

Bars ([M]): 50 % in 40-80 ms, 90 % ("complete") in 0.5-5 s, the 90 % time
lengthening with the time held (2.6x from 1 to 20 s) and with the depth
(1.7x from 5 to 20 dB). The same times at 44.1 and 96 kHz and in the
standard quality, within 0.3 % (bar 2 %). "Complete release" is not
defined in [M]; 90 % of the reduction is our reading, stated in the bar.

### Attack

Steps from -60 dBFS to the level for 3, 6, 10 and 20 dB of steady
reduction: 63 % of it in 10.8, 4.8, 2.2 and 1.7 ms (90 % in 31, 17, 11
and 7 ms). [M] gives no figure ("very fast attack"); the bar is 20 ms and
that the attack shortens as the step grows, which it does: the cell's
generation rises with the light, far faster than the carriers it already
holds.

### Distortion, response (plot: `docs/images/dsp/opto-aliasing-thd.png`)

- THD+N at 1 kHz, compress, peak reduction 100, up to 20 dB of reduction:
  at most 0.113 % (at -4 dBFS in), under [M]'s 0.35 %. This is the gain
  movement's distortion only; the original amplifier's tubes add their
  own (not modelled, below).
- At low frequencies the cell follows the ripple: 1.0 % at 100 Hz (19.7 dB
  of reduction) and 1.8 % at 50 Hz (16.6 dB), reported.
- Below threshold (-60 dBFS, peak reduction 100): within 0.0002 dB at
  30 Hz, 1 kHz and 15 kHz (bar 0.1 dB, [M]'s response). Eco at peak
  reduction 0 is the input exactly.

### Aliasing, and why the default is 4x

Tones from 1 to 19 kHz at -18 dBFS (EBU R68 alignment) with peak
reduction 85, about 5 dB of reduction: worst non-harmonic energy.

| Quality | 44.1 kHz | 48 kHz | 96 kHz | Bar |
|---|---|---|---|---|
| eco (no latency) | -71.1 dBc | -73.8 dBc | -91.7 dBc | -70 |
| standard (4x) | -123.5 dBc | -116.6 dBc | -152.1 dBc | -90 |
| high (8x) | -149.1 dBc | -149.3 dBc | -152.3 dBc | -90 |

The gain moves with the light's ripple at twice the signal frequency and
its harmonics, and the product of that movement with the signal has
components above Nyquist. At the base rate they fold into the band. 2x
was tried as the default: -89.2 dBc at 44.1 kHz on the 19 kHz tone (the
light's fourth harmonic, 76 kHz, folding to 12.2 kHz at 88.2 kHz and
modulating the tone), over the -90 bar; 4x is the lowest factor that
meets it at every rate. At 18-21 dB of reduction (-6 dBFS, peak
reduction 100, every position) eco reaches -52 to -54 dBc (8 kHz at
48 kHz) and standard -117 to -129 dBc, reported.

### Key, link, hot input

- External key at -6 dBFS on a -40 dBFS input: 17.6 dB of reduction (key
  off: 0.001 dB), the reduction the input itself gets at -6 dBFS. The key
  reaches the side chain through the same attenuator share as the tap,
  as through a second photoresistor matched to the first (the original
  lights one for its meter, [M] p. 11), so the loop keeps its feedback
  character on a key, and a key that is the input is exactly the switch
  off. That is what ADR-0011 Amendment 2 asks of a key the host routes
  nothing to: with a feed-forward key the two would differ, which the
  format layer's `native_format_nokey_vst3_opto*` tests caught (8124 of
  8192 samples) before this design.
- Linked, -6 and -40 dBFS: the same reduction on both, 13.2 dB, within
  4e-9 dB. Unlinked, the quiet channel 0.001 dB.
- Squares at 0, +12 and +24 dBFS, peak reduction 100, gain +40 dB, every
  quality, both positions, full emphasis, and anti-phase with the link
  off: finite, the deepest reduction 40.3 dB (the cell's floor under the
  driver's full swing; [M]: up to 40 dB; bar 40.5), the worst block
  8.5-10 µs eco, 38-44 µs standard, 79-87 µs high.

### Common checks

Latency measured equals reported in every quality, at 44.1, 48 and
96 kHz, with and without the key and in limit with full emphasis: eco 0;
standard 76 / 45 / 16 samples; high 78 / 47 / 18. Bypass is the input
delayed by exactly that. Non-finite input, reset on the audio thread,
denormals (silence after signal 0.68 of active), zipper (full-range
sweeps of every automatable parameter: worst -77.9 dBFS, the gain; bar
-60), golden files at 44.1, 48 and 96 kHz.

### CPU (§3.5, class A: p99.9 20 µs, max 50 µs)

`tools/dsp-cpu.sh build-plugins 60 opto`, per q128 stereo block, per-block
minimum of 5 runs over 60 s, on the programme signal at the golden
settings and on the programme raised 12 dB at full peak reduction. Two
sessions on the same machine, the second after a host reboot (ranges
cover both, and both signals):

| Quality | p50 | p99.9 | max | Held |
|---|---|---|---|---|
| eco (no latency) | 8.4 µs | 8.7-9.1 µs | 9.2-9.4 µs | yes, class A |
| standard (4x) | 41.2-45.5 µs | 42.3-46.6 µs | 43.1-49.3 µs | reported |
| high (8x) | 79.7-87.7 µs | 81.3-93.2 µs | 84.8-97.0 µs | reported |

Runner-calibrated (`--calibrated`, as the nightly gate runs it), second
session: the reference kernel 32.586 µs before and 32.584 µs after the
loads (0.006 %), scale 0.9995; eco 8.7-8.8 µs p99.9 and 9.2-9.3 µs max
against 19.99 and 49.98 (the first session: 9.2 and 9.7 µs, scale
0.9997): about 0.27 reference kernels. The nightly's gating step now holds the opto's
eco beside the tube's.

The cost is one chain of dependent operations per sample (the cell's
state sets the tap, the tap the light, the light the state): about 33 ns
per channel-step, the EL law's square root, division and exponential the
largest part (17 ns). Taking the traps off that chain and the divisions
out of the cell bought 5-10 %; standard sits at 2.1-2.3x the class bar,
beside an eco that meets it with room (§3.5, as the tube's standard).

### Bundles (`tools/build-plugins.sh`)

`sonora-opto.lv2`, `sonora-opto-mono.lv2`, `sonora-opto.vst3` and
`sonora-opto-mono.vst3`, built by the format layer of N1:
- lv2lint 2/2; the VST3 validator 47/47 on each of the two;
- 22 renders through `sonora-plugin-host` (LV2 and VST3: golden at 44.1,
  48 and 96 kHz, bypass, automation, mono, key, key mono, nokey, nokey
  mono, and the re-activation for a quality change) bit-exact against the
  harness's reference renders (`native_host_fixture_opto`);
- the format layer's own drives (lazy, late, oversize, threads, wide,
  nokey) on both bundles;
- the state saved through HostControl and restored byte for byte
  (`native_lv2_opto_control_state`, `native_vst3_opto_control_state`).

`tools/build-plugins.sh` (Release, the whole root, on main 4be3baf):
488/488 ctest.

## Not measured, and why

- **Against the original hardware.** No unit was measured. The cell's
  constants are fitted to [M]'s published figures, which are ranges, and
  the switch to [S]. The SignalTrain LA-2A recordings (CC BY 4.0) are the
  public hardware reference: 20.7 GB, recorded at several peak reduction
  settings in one position. Comparing the gain-reduction envelope at the
  dataset's settings, and refitting the cell's constants to it if it
  disagrees, is a follow-up (BACKLOG); a fit to recordings of the
  hardware is the method of §3.3, not a black-box clone, and the
  researcher should confirm the dataset's terms cover the refit.
- **The amplifier.** The original's 12AX7 voltage amplifier, 12BH7
  cathode follower and transformers colour the sound; [M] gives under
  0.35 % THD at +10 dBm. Modelled with slice 2's triode at 2x, it would
  cost about class B; a follow-up with its own SPICE cases (BACKLOG).
- **The EL panel's persistence.** The phosphor's light lags the field;
  no published figure for this panel was found, so it is left out. With
  a decay time of a millisecond it would also low-pass the ripple that
  sets the aliasing above.
- **Noise and hum** are slice 5.

## For the coordinator

- The FET peak limiter did not fit this PR: it is BACKLOG's next item,
  with its plan.
- Standard (4x) is over class A at 42-47 µs per stereo block, beside eco
  at 9 µs that meets it (§3.5's rule, as the tube's standard). The
  optimisation (two channels in SIMD lanes; a tabulated EL law) is in
  BACKLOG.
