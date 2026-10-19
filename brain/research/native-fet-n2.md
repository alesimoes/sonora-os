# FET, stage N2 slice 6b: the FET peak limiter

- Date: 2026-10-14
- Author: dsp-engineer
- Scope: ADR-0011 §6 N2, slice 6b of `planning/BACKLOG.md`:
  - the FET gain-reduction stage and its side chain in `common/dsp`
    (`fet.h`);
  - its SPICE reference (`plugins/native/measure`, `spice_fet.h`);
  - the processor on it (`plugins/native/fet`, "Sonora FET"), with its
    LV2 and VST3 bundles, stereo and mono, with the key input.

  The original's class-A output stage and transformers are a follow-up
  (below).
- Where: the `sonora-plugins` image (Debian trixie, GCC 14, ngspice 44.2)
  on the owner's laptop (11th Gen Intel Core i7-11800H, 2.30 GHz), under
  Docker Desktop on Windows, Release build. CPU figures are container
  figures, not appliance figures (ADR-0011 Amendment 1 item 4).

The original unit is the 1960s FET limiting amplifier the owner named
(the Universal Audio 1176LN, revisions D and E). This note names it
because it is a research note (`native-plugin-suite.md` §5); nothing in
the product does: the processor is "Sonora FET", its category "FET peak
limiter", and the source code cites the manual without the name.

## Sources

| Ref | Source | Used for |
|---|---|---|
| [M] | Universal Audio, *Model 1176LN Solid-State Limiting Amplifier* (reissue manual), part 65-00046, © 2009; copy read at https://kmraudio.com/cdn/shop/files/1176ln_manual.pdf (the zzounds copy `native-plugin-suite.md` §1.6 cites refused fetches; this is the same manual) | "1176LN Circuit Details": Fig. 2 (input section), Fig. 3 (gain reduction stage: R5 27k, Q1, the gate through R7 2.2M \|\| C6 200p from C22 0.22u, and through R6 2.2M \|\| C3 200p and C9 0.22u from R30 6.8k / R86 100 at the preamplifier's output), Fig. 4 (R5 and Q1 "as a voltage divider which controls the gain"), Fig. 5 (gain reduction control circuit: R6 1k into R19 68k, R20 56k, R21 56k, R22 47k and the ratio switches' A sections; Q7-Q10 2N3707 with R38 47k, R39 4.7k; CR2, CR3; R54 470 \|\| C27 0.022u, R55 25k attack; R56 5M, R57 270k release; the B sections on R76 150, R75 470, R73 560, R68 1.5k, R64 1.5k; R35 10k to -10 V; R59 2k "Q bias") |
| [M] text | Same, pp. 8-33 | Attack 20-800 us, release 50 ms-1.1 s, faster clockwise; ratios 4, 8, 12, 20; "higher Ratio settings also set the threshold higher"; all-button mode: "distortion increases radically due to a lag time on the attack of initial transients", "the ratio goes to somewhere between 12:1 and 20:1, and the bias points change all over the circuit"; "the 1176LN utilizes soft knee"; "a feedback style compressor since the sidechain circuit samples the signal level after the gain reduction"; "the greater the voltage applied to the gate of the FET, the less resistance"; the LN circuit "designed to ensure that the FET stayed as much within a linear region as possible"; the bias "applied to the diodes by the Compression Ratio pushbutton switches ... controls the threshold"; R55 "regulating how fast C22 is charged", R56 "the rate at which C22 discharges"; the "Q" bias calibration ("adjust R59 CW until the output decreases by 1.0 dB"); the stereo adapter: "the fastest attack time is doubled (that is, it becomes 40 microseconds instead of 20 microseconds" |
| [M] spec | Same, p. 38 | "Output of +22 dBm with no greater than 0.5% T.H.D." "with limiting, at 1.1 seconds release setting", "50 Hz - 15 kHz"; response 20 Hz-20 kHz +-1 dB; gain 45 dB |
| [SH] | H. Shichman and D. A. Hodges, "Modeling and simulation of insulated-gate field-effect transistor switching circuits", IEEE JSSC 3(3), 1968 | The square-law channel with channel-length modulation, as SPICE's JFET level 1 (ngspice NJF, B = 1) |
| [J] | National Semiconductor's 2N5457 SPICE model (1988), as distributed in the vendors' libraries and quoted at diyaudio.com (thread on 2N5457 models; the forum refused direct fetches, read through search summaries) | BETA 1.125 mA/V^2, VTO -1.372 V, LAMBDA 2.3 mV^-1. IDSS 2.1 mA and VGS(off) 1.37 V, inside onsemi's 2N5457 datasheet ranges (1-5 mA, 0.5-6 V). **Unverified** against an official file |
| [J'] | onsemi's MMBF5457 SPICE model (2002), quoted in the same thread | BETA 307 uA/V^2, VTO -3.87 V, LAMBDA 4.5 mV^-1: tried, and not used (below) |
| [D] | The 1N4148 SPICE model (IS 2.52 nA, N 1.752), as in the common SPICE libraries | The rectifiers CR2, CR3, whose type [M] does not give |
| [Y] | D. T. Yeh, J. S. Abel and J. O. Smith, "Automated physical modeling of nonlinear audio circuits for real-time audio effects, Part I", IEEE TASLP 18(4), 2010 | The method: the linear network eliminated symbolically, Newton on the nonlinear ports |
| | Nagel, SPICE2 (1975), `pnjlim` | The junction step limit in the Newton loops |
| | EBU R68 | 0 dBu = -18 dBFS, for the alignment level |
| | ngspice 44.2 (BSD-3-Clause) | The SPICE reference |

Not reached, as BACKLOG foresaw for validation figures only: the Aveiro
thesis (H. Antão, 2014; the repository reset the connection) and Gerat,
Eichas and Zölzer (AES 143, 2017; paywalled). No validation figure in
this note comes from them.

The manual's text and drawings are copyrighted: the circuit values are
facts re-entered as our own netlist (`spice_fet.cpp`), and no drawing or
text is redistributed. No third-party code, no product's or plugin's
output used as a reference or fitting target, no preset copied (ADR-0004,
ADR-0011 §3.3, §5).

## The model

```
vin --R5 27k--+-- drain -> preamplifier (Gp) -> output
              |
              Q1 (2N5457 [J]), gate = (Vc + Vq) / 2 + vd / 2
              |
  gate <- R7 2.2M <- C22 (Vc)          gate <- R6 2.2M <- C9 (Vq) <- R86 null
preamplifier -> R6 1k -> ratio string tap k_r -> Q7 (A = R38 / R39) ->
  CR2, CR3 at Vb_r +- s -> R54 || C27 -> R55 -> C22 -> R57 + R56 -> rest
```

**From [M], as published:** every resistor and capacitor above; the
topology of Figs. 3 and 5; the 1.0 dB rest loss (the "Q" bias
calibration), which the model holds at every ratio and release by solving
the rest point with the rectifiers' leakage.

**From device physics:** [SH] for the FET, odd in vds; Shockley for the
rectifiers, with the second diode's term (both conduct near zero drive).

**The LN circuit, worked out.** The gate sits at the middle of two equal
arms. With R86 trimmed so the gate gets half the drain's own audio, [SH]'s
ohmic current beta vds (2 (vgs - vto) - vds) becomes 2 beta (Vg0 - vto)
vds: the channel is a linear resistor for either sign of vds, which is
what [M] says the circuit was for. What distortion is left is
channel-length modulation, the saturated region at light reduction, and
the control voltage's ripple (fast release).

**Assumed, because [M] gives no figure** (each a parameter of
`FetCircuit`):

| Value | Taken as | Why | Would be replaced by |
|---|---|---|---|
| Gp, the preamplifier's gain from the drain | 69 | the gain at which R86 at its 100 ohm end nulls with R30 6.8k: the smallest the trim can null | the preamplifier's measured gain; only the side chain's sensitivity depends on it |
| A, the side chain's gain | 10 | Q7's R38 / R39; the followers at unity | the side chain's measured gain |
| Bias string drive | 10 V | the figure draws R35 to the -10 V supply | the string's measured voltages |
| Followers' swing | +-12 V | the side chain's supply | the measured clip |
| Gate forward limit | 0.5 V | a silicon junction fed through 1.1 Meg (R6 \|\| R7) | the FET's gate current |
| Full-scale level | 0.0126 V at R5 | our alignment: 4:1 reduces by 1 dB at -20 dBFS at input 0 dB, so [M]'s getting-started level (0 VU, +4 dBu, -14 dBFS under R68) gets a few dB | none needed: an alignment, not a property |

**Why [J], not [J'].** The ratio a button gives is set by the bias
string's offset times the channel's conductance per volt of gate, so by
BETA. In the exploration before this design settled (A = 4, the earlier
lagged solve; A moves the threshold, not the ratio), [J'] gave peak
ratios of 2.5, 4.0, 5.8 and 10.8 across the buttons with the string near
10 V, and [J] 3.1, 5.5, 8.5 and 17.0. Both are published 2N5457 models
inside the datasheet's ranges; [J] is the one under which the published
values come nearest the labels. That choice is a fit of a device model to
the labels, and it is recorded as one.

**Our readings, Unverified until the original service schematic or a
unit settles them:**
- *The switch wiring.* Fig. 5 is a simplified drawing. We read the A
  sections as choosing a tap of the ratio string (20:1 at its top, 4:1
  above R22) and the B sections as choosing a tap of the bias string
  (4:1 nearest the rest node, after R76), each button's tap further along
  for a higher ratio. Pressing all four then shorts R19-R21 and R75-R68:
  the side chain at 20:1's level, the bias at R76's tap, with more current
  in the shortened string.
- *C9.* As printed (0.22 uF, like C22), C9 behind R6 + R7 (4.4 Meg) makes
  the gate follow the control voltage half at once and half over about a
  second. This sets the release's character (below). A smaller C9 on the
  original, or a DC return for the R6 arm, would remove the slow stage.

**The stereo link** joins the two C22 nodes (the stereo adapter), so one
channel's rectifier charges both: [M]'s "fastest attack time is doubled".

## The processor

"Sonora FET" (`org.sonora.fet`, category Dynamics), stereo and mono, with
a key input. Parameters, append-only: input (-24 to +24 dB, the level
into the divider; it raises the output too, as on the original), output
(-24 to +24 dB; 0 and 0 is unity below threshold), attack (the attack
network's time constant, (R54 + R55) C22, 0.10 to 5.6 ms), release (the
release network's, (R57 + R56) C22, 59 ms to 1.16 s), ratio (4, 8, 12,
20, all buttons; smoothed over 20 ms), stereo link, mix (ours), external
key, quality (eco 1x with no latency, standard 4x by default, high 8x;
reconfigures).

The key reaches the side chain through a second divider on the same
gate, as matched to the first, so a key that is the input is exactly the
switch off (ADR-0011 Amendment 2), and a keyed input is reduced as the
key would be.

## Solving it

Per sample, at the sample's instant: the capacitors (C22, C9, C27) are
trapezoidal and the linear network is eliminated symbolically [Y], which
leaves two unknowns per channel, the drain and the rectifier's junction,
solved together by Newton (four for a linked pair: the Newton step by a
Schur complement on the junctions, a 2 x 2), with SPICE's junction step
limit and the drain bracketed. What it costs was worked down against the
CPU bar, each step measured:
- starts close enough that one Newton correction usually suffices: the
  drain from the ohmic channel with lambda at the gate extrapolated from
  its last two samples, the junction moved with the drive by the
  rectifier's own sensitivity; and a stop on the correction's size
  (1e-4 V on the junction, 1e-5 of the drain; quadratic convergence
  leaves about 1e-7 V and 3e-8 of the drain). On programme at 8:1 the
  mean went from 1.94 iterations a sample to 1.07;
- when the rectifier is off (both junctions more than 12 nVt reverse,
  where its current is -2 Is within 7e-6 of Is) the step is explicit and
  takes no iteration and no exp;
- the Newton step's Schur complement with one division, not three; the
  channel's closed form by its series in lambda, without a square root.

`dsp_fet`'s step from 0.01 V to 0.5 V at 20:1 takes at most 10 iterations
(the cap is 60). A step that reaches the cap commits its last point and is
counted (`FetChannel::cap_hits`): `dsp_fet` and the suite's `newton_cap`
hold the count at zero on hot squares, mono and linked, at every
quality's rate.

**The cost at the input ceiling (RT audit of #74, B1).** ProcessorBase
accepts input up to 1e6 FS, which with Input at +24 dB is about 2e5 V at
R5. There the drain's ohmic start, proportional to the drive, sat far
above the saturated channel's root (near its square root), and Newton
halved its error each iteration: 16 a step on average at the ceiling,
and high (8x) missed the period at 96 and 192 kHz. Two fixes, both
measured by the suite's `newton_cap` and `hot_input`, which now drive to
999999 FS at Input +24 dB at every quality:
- the start uses the saturated law's closed form whenever the ohmic one
  breaks a < 2 v0, so the count stays flat in the drive (at the ceiling
  a mean of 1.03 iterations a step, at most 9);
- |vin| at R5 is bounded at 30 V (`FetCircuit::input_limit_v`, an
  assumption: no voltage in the unit exceeds its supply), ten times the
  hottest the other checks drive, so nothing inside the tested range
  changes (the goldens and the SPICE comparison are bit-exact). The
  processor reports a block that reached it, or a step that hit the cap,
  as bounded (ProcessorBase's `note_bounded`).

Worst block after the fix: stereo linked, q128, Input +24 dB, 20:1,
fastest attack and release, a 997 Hz square, 800 blocks, the minimum of
3 runs per block. Taken on a quiet host (load average 0.24) in the
sonora-plugins container; the RT audit's table had high at 1.8 to 1.95 ms.

| base rate | quality | 0 dBFS: median / p99 / worst | 999999 FS: median / p99 / worst | period |
|---|---|---|---|---|
| 48 kHz | standard (4x) | 84 / 106 / 123 us | 79 / 81 / 92 us | 2667 us |
| 48 kHz | high (8x) | 158 / 167 / 172 us | 149 / 157 / 160 us | 2667 us |
| 96 kHz | standard (4x) | 76 / 78 / 82 us | 72 / 74 / 76 us | 1333 us |
| 96 kHz | high (8x) | 150 / 157 / 160 us | 144 / 150 / 156 us | 1333 us |
| 192 kHz | standard (4x) | 74 / 78 / 82 us | 72 / 74 / 80 us | 667 us |
| 192 kHz | high (8x) | 144 / 152 / 173 us | 141 / 149 / 154 us | 667 us |

A step whose point is not finite (a NaN device parameter is the only way
the tests find to force one) is never committed: the channels solved
together reset to rest, and the step is counted with the cap hits.

The first version took the drain from the gate of the sample before and
solved the rectifier after it: a sample of delay in the feedback loop.
Against ngspice it read late on every attack, 0.04 dB at 192 kHz on a
1 kHz burst and 5.5 dB on the fastest attack's 10 kHz burst; solving the
two together took those to 0.0017 and 0.10 dB.

## Results (`native_fet`, `dsp_fet`, `spice_fet_*`, `tools/build-plugins.sh`)

### Against ngspice (plot: `docs/images/dsp/fet-vs-spice.png`)

Gain reduction through six tone bursts (quiet -60 dBFS, a step up, back
to quiet; 10 us grid for 2 ms after each step), max |model - ngspice|, dB;
ngspice runs the same reduced circuit as the model, with its own JFET
and diode models: R5, the JFET, the diodes and the passive networks (C22,
C9 behind R6 + R7, R54 || C27, R55, the release resistor) are ngspice
elements, while the gate (the LN midpoint), the side chain's gain, swing
and phase splitter, the bias taps and the rest point are behavioural
sources. So it checks the solver and the device laws, not our reduction
of Figs. 3 and 5 to that circuit; a component-level netlist of the
figures is a follow-up (BACKLOG, N2 FET follow-ups (i)).

| Case | eco 44.1 / 48 kHz | standard: 176.4 / 192 / 384 kHz | bar |
|---|---|---|---|
| 4:1, attack 0.5 ms, release 59 ms, 1 kHz, -6 dBFS (7.1 dB) | 0.039 / 0.035 | 0.0030 / 0.0017 / 0.0005 | 0.05 |
| 8:1, slowest attack and release, 1 kHz, 0 dBFS (14.4 dB) | 0.064 / 0.059 | 0.0025 / 0.0024 / 0.0024 | 0.05 |
| 12:1, 100 Hz, 0 dBFS (13.5 dB) | 0.021 / 0.018 | 0.0016 / 0.0012 / 0.0012 | 0.05 |
| all buttons, 1 kHz, -12 dBFS (13.3 dB) | 0.160 / 0.156 | 0.011 / 0.012 / 0.0025 | 0.05 |
| 20:1, fastest attack, 10 kHz, +6 dBFS (16.3 dB) | 4.04 / 3.79 | 0.25 / 0.10 / 0.050 | 0.3 |
| 20:1, fastest attack and release, 1 kHz, +18 dBFS (26.7 dB) | 0.90 / 0.97 | 0.11 / 0.053 / 0.027 | 0.3 |

The two fastest-attack cases are held to their own bar: their error sits
in the first microseconds after the step, where R54 C27 (10 us) is two
steps at 176.4 kHz; high (8x) takes them to 0.035 and 0.063 dB at
352.8 kHz. The eco processor at 48 kHz matches the circuit it wraps
exactly (max |difference| 0, held to 1e-7 FS). The references regenerate
in 75 s; each `spice_fet_*` ctest re-runs one case and holds the netlist
byte for byte and the trajectory to 1e-5 dB.

### Static curve, ratio and threshold (plot: `docs/images/dsp/fet-static-curves.png`)

1 kHz, standard, attack 0.5 ms, release 250 ms, 6 s settled:

| Button | 1 dB of reduction at (input 0 dB) | steepest ratio, 3-15 dB of reduction | against the label |
|---|---|---|---|
| 4:1 | -20 dBFS | 3.41 (at 12.2 dB) | -15 % |
| 8:1 | -16 dBFS | 6.10 (at 9.6 dB) | -24 % |
| 12:1 | -14 dBFS | 9.38 (at 9.2 dB) | -22 % |
| 20:1 | -10 dBFS | 18.1 (at 8.2 dB) | -10 % |
| all buttons | -32 dBFS | 3.58 (at 12.2 dB) | [M]: 12-20 |

Held: each between 0.65 and 1.1 times its label; the threshold rising
with the ratio ([M]); standard against high within 5e-6 dB. The ratio
falls off at depth (to 2.2-10 at 20-29 dB of reduction): the channel's
conductance is linear in the control voltage, and a feedback loop around
a linear law tends to 2:1. The knee is soft ([M]: "soft knee").

**All buttons.** Under our reading of the switch it lowers the
threshold 12 dB below 4:1's at about 3.6:1, heavy compression from a low
threshold, where [M] says "between 12:1 and 20:1" and that the bias
points change. Reported, not held, and the first thing a traced switch or
a unit would settle (BACKLOG).

### Attack (plot: `docs/images/dsp/fet-attack-release.png`)

t63 of the reduction, 20:1, a 10 kHz burst from -60 dBFS, on the circuit
at 768 kHz (interpolated; 192 kHz quantised it to 5.2 us):

| Step to | reduction | fastest (0.10 ms) | slowest (5.6 ms) | fastest, linked |
|---|---|---|---|---|
| -5 dBFS | 6.2 dB | 19.1 us | 380 us | 20.9 us |
| 0 dBFS (held) | 10.9 dB | 14.7 us | 272 us | 16.5 us |
| +10 dBFS | 20.0 dB | 10.3 us | 214 us | 12.5 us |

Held at 0 dBFS: fastest within 5-40 us ([M]: 20 us), slowest within
200-1600 us ([M]: 800 us), monotonic over five settings. The slowest is a
third of [M]'s figure on this step, and nearer half at -5 dBFS: the
published R55 (25k) and C22 give a 5.6 ms network, which a hard-driven
rectifier crosses in a fraction; how [M] measured its 800 us is not
stated. A DC step shows the other end: C27 couples a step's first part
onto C22 at once (t63 under 1 us at the fastest setting).

**The link.** Joined, one channel's rectifier charges both C22 nodes:
C22's rise over the first 30 us of a DC step, linked over alone, 0.503
(`dsp_fet`, held to 0.45-0.55): [M]'s doubling, on the network. On the
burst the reduction's t63 grows only 1.1-1.3x, because there the burst's
peaks, not the network, set the fastest attack.

### Release

| Release control | control voltage, 1/e | reduction to half (from about 11 dB, 20:1) |
|---|---|---|
| 59 ms | 60.6 ms | 358 ms |
| 250 ms | 273 ms | 856 ms |
| 1.16 s | 1.72 s | 2.92 s |

The control's range follows from the published values: 59 ms to 1.16 s
against [M]'s 50 ms to 1.1 s. The control voltage decays at the
control's value (held within 0.9-1.6 times; at the slowest C9's path,
which feeds C22 from the lagging C9, lengthens it 1.5x). The reduction
recovers in two stages: the gate drops by half the control voltage's
excursion at the network's rate, and the other half follows C9 over about
a second. At depth the second stage dominates, so from 11 dB at the
fastest setting the reduction takes 358 ms to halve. **This is the
model's largest departure from the original's reputation for fast
release**, and it follows directly from C9 as printed; it is Unverified
and in BACKLOG with what would settle it.

### Distortion, response (plot: `docs/images/dsp/fet-aliasing-thd.png`)

- THD+N at [M]'s condition (the slowest release, with limiting, 50 Hz to
  15 kHz, up to 20 dB of reduction): at most 0.074 % (20:1, 50 Hz, 10 dB),
  bar [M]'s 0.5 %. This is the gain cell's own; the output stage [M]
  rates at +22 dBm is not modelled (below).
- At the fastest release the control voltage's ripple modulates the gain
  and THD+N at 1 kHz rises with the reduction: 0.005 to 0.070 % (4:1) and
  0.011 to 0.096 % (20:1) from 1 to 20 dB. Held: it rises.
- Below threshold the divider is resistive: within 0.0001 dB at 20 Hz,
  1 kHz and 20 kHz (bar 0.1 dB).

### Aliasing, and eco at high frequencies

Tones 1-19 kHz at -18 dBFS, input +6 dB, 4:1 (4.6 dB of reduction at
1 kHz): worst non-harmonic energy.

| Quality | 44.1 kHz | 48 kHz | 96 kHz | Bar |
|---|---|---|---|---|
| eco (no latency) | -95.3 dBc | -75.5 dBc | -119.4 dBc | -70 |
| standard (4x) | -148.2 dBc | -148.5 dBc | -152.3 dBc | -90 |
| high (8x) | -149.0 dBc | -149.3 dBc | -152.3 dBc | -90 |

Reported, heavier: 20:1 at input +18 dB with the fastest attack and
release (10.8 dB at 1 kHz): eco -71.4 / -34.3 / -49.5 dBc, standard
-93.7 / -75.2 / -107.5 dBc, high -111.1 / -107.7 / -121.6 dBc at 44.1 /
48 / 96 kHz. The fastest release makes the gain follow the rectifier's
ripple, and at 48 kHz an 8 kHz tone's products fold into the band. High
is the mode for that.

**Eco at high frequencies** (the lesson of PR #57's review). Eco's
rectifier sees a high tone only at the samples; on a tone locked to the
rate the reduction depends on the tone's phase against them. 20:1, nine
phases, eco against standard (`eco_high_frequency`):

| Tone | -8 dBFS | -4 dBFS | 0 dBFS | bar |
|---|---|---|---|---|
| 12 kHz at 48 kHz (fs/4): eco over the phases, standard | 1.23-3.80, 3.56 dB | 4.85-7.55, 7.31 dB | 8.61-11.32, 11.09 dB | |
| max \|eco - standard\| (11.025 kHz at 44.1 kHz the same) | 2.33 dB | 2.46 dB | 2.48 dB | 2.6 |
| 8 kHz at 48 kHz: max \|eco - standard\| | 0.84 dB | 0.86 dB | 0.87 dB | 0.95 |

At fs/4 eco samples the peaks at best and at 0.707 of them at worst, so
it reduces up to 2.5 dB less than standard; a tone not locked to the
rate walks through these phases, or beats near fs/4. Documented, not
fixed: an inter-sample peak estimate needs the next sample, which eco's
zero latency does not have, and running eco's side chain at 2x doubles
the cost class A is held to. Standard is the default and the mode for
limiting high-frequency peaks; the bars hold eco at what it measures, so
a regression shows.

### Key, link, hot input

- External key at -6 dBFS on a -40 dBFS input, 4:1: 8.69 dB of
  reduction, the -6 dBFS signal's own reduction within 2e-5 dB (key off:
  0.009 dB).
- Linked, -6 and -40 dBFS: the same reduction on both within 3e-5 dB;
  unlinked, the quiet channel 2e-5 dB.
- Squares at 0, +12 and +24 dBFS with the input at +24 dB, every quality,
  20:1 and all buttons with the fastest attack and release, and
  anti-phase with the link off: finite; the deepest reduction 40.45 dB,
  at the gate's forward limit (40.2 dB small-signal; bar 40.7); the
  worst block at most 1.9 times the median
  block on a -60 dBFS square in the same configuration (bar 6x, the
  tube's and the opto's).

### Common checks

Latency measured equals reported in every quality, at 44.1, 48 and
96 kHz, with the key and with all buttons at the fastest attack: eco 0;
standard 76 / 45 / 16 samples; high 78 / 47 / 18. Bypass is the input
delayed by exactly that. Non-finite input, reset on the audio thread,
denormals (silence after signal 0.67 of active), zipper (full-range
sweeps of input, output, attack, release, ratio, link and mix: worst
-80.8 dBFS, the ratio; bar -60), golden files at 44.1, 48 and 96 kHz.

### CPU (§3.5, class A: p99.9 20 us, max 50 us)

`tools/dsp-cpu.sh --calibrated build-plugins 60 fet`, per q128 stereo
block, per-block minimum of 5 runs over 60 s, on the golden configuration
(8:1, input +12 dB, about 10 dB of limiting) and on the programme raised
12 dB at 20:1 with the fastest attack and release. i7-11800H, Docker
Desktop, after follow-up (e) (2026-10-18). Taken on a quiet host: a script
waited until no other container ran (33 minutes), checked every 30 s
during the run that none started, and discarded the run otherwise; load
average 3.3 at the start (falling from the builds before) and 1.3 at the
end. The reference kernel read 32.607 us before the loads and 32.585 us
after (0.07 % apart), against 32.6 us recorded for this machine, so the
machine ran at its reference speed and the calibrated bar is the class
bar (x 0.99988). A run the same afternoon that a neighbour's build
interrupted read eco 21.6 us p99.9 from a quiet start and was discarded.

| Quality | golden: p50 / p99.9 / max | programme +12 dB, 20:1, fastest | Held |
|---|---|---|---|
| eco (no latency) | 16.8 / **19.66** / 19.9 us | 11.4 / 16.6 / 17.3 us | class A: meets it, **1.7 % under** on golden |
| standard (4x) | 72.8 / 80.3 / 82.9 us | 52.2 / 65.2 / 68.2 us | reported |
| high (8x) | 138.0 / 147.3 / 152.0 us | 99.3 / 120.9 / 124.5 us | reported |

PR #74 measured eco at 17.4 / 20.5 / 20.7 us. **Eco now meets class A,
but without margin**: 19.66 us against 20. The FET stays out of the
default insert list and out of the nightly CPU gate (a runner that tracks
the reference kernel a few per cent worse would go red on it), and
`cpu.p999_kernels` is not recorded (BACKLOG, N2 FET follow-ups (e)).

**Where the time goes** (golden, eco, the circuit alone, ablations with
the iteration count forced, timed A/B block by block so a loaded host's
noise falls on both): everything outside Newton plus the first current
evaluation about 7.9 us a block; the first Newton step about 5.5 us; each
further iteration about 5.1 us; exp about 0.75 us of an iteration;
`render` and `ProcessorBase` about 1.5 us. The mean is 1.42 iterations a
sample; the p99.9 blocks are where the programme's sweep is at 20-40 Hz,
at about 1.8: whole blocks near the drive's slow zero crossings. Per
channel the first iteration's stop fails no more often there (junction
25.6 %, drain 17.8 %, against 25.7 % and 18.0 % elsewhere); the two
channels fail together more often.

**Kept** (each the same bits as before on eco's golden and hot programmes,
and the golden files unchanged):
- `std::fmin`/`std::fmax` as comparisons (`min_of`, `max_of`, the same
  value whenever the second operand is not NaN): GCC calls libm for them
  without -ffinite-math-only, and each call clobbers every xmm register
  (SysV ABI), so the solve's state was spilled around six calls a sample;
- the channel law inline with its slopes by reference (it was the solve's
  largest callee, 117 M instructions on 20 s of programme);
- the processor's peak tracking as comparisons (two `fmax` calls a
  sample), and the link and mix smoothers read once a block while
  settled.

Callgrind counted the FET's instructions down from about 1550 M to 1190 M
on 20 s of programme (23 %); block-alternating A/B of the circuit alone
under load read 0.85-0.89 of #74's time; the full processor at reference
speed, about 0.96. Instructions were not the binding cost.

**Measured and not kept** (each A/B against the kept code, or the full
harness), so the next attempt starts past them:
- *a stop on the error left rather than on the correction's size* (a
  second-order bound on the drain's and the junction's error after the
  step): 1.42 to 1.31 iterations, accuracy unchanged, but slower: the
  bound costs what it saves. The junction mostly failed it at 8:1 because
  the linearised current's error is dy^2 / 2 of the forward current
  however linear the junction's equation is;
- *starts from the linear network* (the gate from P + Q I_last, the
  junction from its full residual): 1.42 to 1.37 iterations (all buttons
  2.02 to 1.45), slower, and the p99.9 blocks unchanged (1.74);
- *the drive's sensitivity in the junction's start* (the lower junction's
  dI/ds): no change in any count;
- *a chord second iteration* (the first Jacobian's factors reused, the
  step's distance from Newton's bounded): 96 % accepted, accuracy
  unchanged, no faster (the Jacobian is still formed for the bound);
- *the two channels in two SSE2 lanes* (`solve<2>` transcribed lane by
  lane, the same bits on every test signal, 100 % of programme samples in
  the lanes), with libm's exp per lane and then with a table-driven exp
  (Tang 1989) in both lanes and the portable solve (1 ulp of glibc): 6-8 %
  slower on golden, 9-14 % on the hot programme;
- *an inline table-driven exp* in the scalar solve: fewer instructions,
  no faster than glibc's FMA build;
- *stop tolerances three times looser* (3e-4 V, 3e-5): mean 1.31
  iterations, the p99.9 blocks unchanged at 1.73, and the departure from
  the root ten times larger (1.8e-6 dB). Not a bar, but no gain where it
  counts, so not kept.

`dsp_fet` now holds the solve against the root (the same circuit iterated
to round-off): the drain within 1e-8 of the peak drive and the reduction
within 5e-7 dB on five programmes; today 5.9e-9 and 2.5e-7 dB at most. A
change made for cost cannot trade accuracy away unseen.

What is left, for the follow-up: the p99.9 is set by blocks of two
iterations around slow zero crossings, at about 5 us a block an
iteration, and by a per-sample chain through the state (commit, the
linear step, the starts, the first Newton step) that neither fewer
instructions nor two lanes shortened. Candidates not yet measured: a
solve in the junction's current rather than its voltage (the exponential
inverted, so the rectifier's equation is near linear in conduction), and
a predictor across the zero crossings from the drive's own slope.


## Not measured, and why

- **Against the original hardware.** No unit was measured and no public
  recording of one under controlled settings was found. The circuit is
  the published one; the devices are published models; the ratios, the
  all-button behaviour and the two-stage release are what those values
  give.
- **The switch wiring and C9** (above): the original service schematic,
  or a unit, settles both.
- **The input transformer, the preamplifier's own colour, the class-A
  output stage and its transformer**: linear gains here. [M] credits the
  output transformer with much of the sound; modelling it is the
  transformer slice's (BACKLOG, slice 3) and a follow-up here.
- **Noise and hum** are slice 5.

## For the coordinator

- Two departures from the original's documented behaviour are the
  published circuit's, as read, and are reported rather than tuned away:
  the all-button mode (about 3.6:1 from a low threshold, where [M] says
  12-20:1) and the release's slow second stage through C9. Both are in
  BACKLOG with what would settle them.
- The ratios come out 10-24 % under the buttons' labels with [J]; the
  bar holds them between 0.65 and 1.1 times.
- Eco meets class A's p99.9 on the golden configuration since follow-up
  (e), but by 1.7 % (19.66 us, quiet host at the reference kernel's
  speed). That is not margin: the FET should not enter the default insert
  list or the nightly CPU gate until a follow-up gives it some (the CPU
  section lists what was measured and did not).
