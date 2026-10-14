# Tube drive, stage N2 slice 2: the SPICE reference harness and the triode

- Date: 2026-10-13
- Author: dsp-engineer
- Scope: ADR-0011 §6 N2, slice 2 of `planning/BACKLOG.md`:
  - ngspice in the plugins container;
  - the SPICE reference harness in `plugins/native/measure` (`spice.h`);
  - the triode and its common-cathode stage in `common/dsp` (`triode.h`);
  - the tube drive processor built on them (`plugins/native/tube`), with
    its LV2 and VST3 bundles, stereo and mono.

  The transformer, supply sag and modelled noise are later slices.
- Where: the `sonora-plugins` image (Debian trixie, GCC 14, ngspice 44.2)
  on the owner's laptop (11th Gen Intel Core i7-11800H, 2.30 GHz), under
  Docker Desktop on Windows, Release build. The CPU figures are container
  figures, not appliance figures (ADR-0011 Amendment 1 item 4).

## Sources

| Part | Source | Used as |
|---|---|---|
| Triode law, default | K. Dempwolf and U. Zölzer, "A physically-motivated triode model for circuit simulations", DAFx-11, eqs. (10)-(12), Table 1 column RSD-1 (12AX7) | Ik, Ig with grid current, Ip = Ik - Ig. Continuously differentiable |
| Triode law, second | N. Koren, "Improved vacuum tube models for SPICE simulations", Glass Audio 8(5), 1996, eq. (4), Table 1 (12AX7) | Ip as published; grid current through RGI (2 kΩ) above 0 V, the only form the article gives |
| Interelectrode capacitances | The 12AX7 datasheet values both papers quote: Cgk 2.3 pF, Cgp 2.4 pF, Cpk 0.9 pF | Cgp (the Miller capacitance) in the stage; Cgk and Cpk left out (below) |
| Solver | Modified nodal analysis with trapezoidal companion capacitors (Chua and Lin, 1975), reduced to the two port voltages as in the DK method (Yeh, Abel and Smith, IEEE TASLP 18(4), 2010) | Newton on Vgk, Vpk per sample, at the oversampled rate |
| Oversampling | N0's polyphase half-band FIR (`oversampling.h`) | 2x, 4x, 8x, exact integer latency |
| Reference | ngspice (BSD-3-Clause), transient analysis of the generated netlist | `plugins/native/tube/tests/spice/` |

The equations and parameters are published facts. Koren's model files
carry no licence (`open-data-and-dsp-sources.md`); none was used. No
third-party code. Nothing was taken from any product or plugin, and no
plugin's output was used as a reference (ADR-0004, ADR-0011 §3.3, §5).

The stage is a textbook resistance-coupled amplifier with values of ours:
B+ 250 V, Rp 100 kΩ, Rk 1.5 kΩ with 22 µF, 22 nF and 1 MΩ grid leak in,
22 kΩ grid stopper, 22 nF and 1 MΩ out. Operating point (Dempwolf): plate
170.449 V, cathode 1.1934 V, 0.796 mA, the same as ngspice's `.op` to
1e-7 V. Small-signal gain 62.4 (35.9 dB) at 1 kHz.

**Cgk and Cpk are left out** (review of PR #52, for CPU). Here they set
poles at 3.1 MHz (Cgk across the 22 kΩ stopper) and 4.6 MHz (Cpk across
the plate's ~38 kΩ), which the trapezoidal rule folds to modes near the
oversampled Nyquist frequency rather than models. In ngspice, removing
both moved the harmonics of the 4 V, 5 kHz case by at most 0.01 dB to
20 kHz and 0.16 dB at 45 kHz. Each was a state of the solve. The netlists
and `reference.json` were regenerated without them.

## The harness

- The netlist is generated from the same `TriodeStageCircuit` and
  `TriodeDevice` values the model is built from, the laws as ngspice
  B-sources. The generated netlists are committed; `spice_triode_*`
  (ctest label `spice`, only where ngspice is installed) checks that they
  are byte-identical to what the model's values generate, re-runs
  ngspice, and checks the harmonics against the committed
  `reference.json` to 1e-6 of the fundamental and the operating point to
  1e-6 V (they match to 0 on ngspice 44.2). Each of the 16 cases is its
  own ctest; about 3.5 minutes of CPU, under 1 in parallel.
- `abstol` is 100 pA: with Cgp the grid's only capacitance, 1 fA stopped
  ngspice at its first time point on the Dempwolf 5 kHz cases, and 1 pA
  the Koren ones (whose grid law has a kink at 0 V).
- Settling 0.3 s, then 0.05 s analysed: whole periods at 100 Hz, 1 kHz and
  5 kHz. Maximum step 1/(2000 f0); reltol 1e-7. Halving the step to
  0.1 µs moved the 5 kHz, 2 V case's harmonics above -60 dBc by 0.2 dB at
  most at 0.5 µs, which is why the step follows f0.
- The harmonics come from integrating ngspice's piecewise-linear waveform
  against e^(-j 2π k f0 t) exactly over its own time points. The model's
  come from a DFT over the same window. Both are complex, so phase is
  compared too.
- The model's suite reads the committed reference, so the comparison runs
  wherever the tests run, with or without ngspice.

## Results (`native_tube`, `dsp_triode`, `tools/build-plugins.sh`)

Plot: `docs/images/dsp/tube-harmonics-vs-spice.png`.

| Measure | Result | Bar |
|---|---|---|
| Operating point vs ngspice, both laws | ≤ 3.2e-7 V | 1e-3 V |
| Fundamental vs ngspice, 16 cases at 176.4, 192 and 384 kHz | ≤ 0.0036 dB | 0.05 dB |
| Harmonic levels vs ngspice, every harmonic ≤ 20 kHz and ≥ -80 dBc | ≤ 0.087 dB (Koren, 4 V, at 192 kHz); Dempwolf ≤ 0.05 dB | 0.5 dB |
| Waveform error to 20 kHz, \|model - SPICE\| / \|H1\|, complex | ≤ -52.9 dB (Dempwolf, 4 V, 5 kHz, at 176.4 kHz) | -40 dB |
| The same at 88.2 kHz (eco at 44.1 kHz), reported | -40.5 dB | — |
| Processor through the oversampler, harmonic levels vs ngspice, 12 Dempwolf cases at 44.1, 48, 96 kHz | ≤ 0.054 dB | 0.5 dB |
| Tabulated law against the exact one, through the stage | -180 dB | -120 dB |
| Small-signal gain at 1 kHz, drive 0 dB, auto level | within 1.5e-5 dB of unity | 0.05 dB |
| Small-signal response at 20 Hz, 100 Hz, 10 kHz, 20 kHz (the coupling capacitors and the Miller pole), reported | -1.74, -0.07, -0.20, -0.79 dB at 48 kHz | — |
| Latency, measured - reported, eco/standard/high at 44.1/48/96 kHz | 0 (69/39/11, 76/45/16, 78/47/18 samples) | exact |
| Bypass = input delayed by the latency | exact | exact |
| Non-finite and 1e30 input | finite; identical to silence in their place from the next block | §3.2 |
| Denormals: silence after signal / active | 0.62 | 1.2 |
| Zipper, full-range sweeps of drive, output, mix, auto level, -40 dBFS tone | ≤ -120.5 dBFS | -94 dBFS (-60 moved with the tone) |
| Golden renders 44.1/48/96 kHz | identical | 1e-6 |
| Newton iterations per sample, most: 1 V / 4 V / 63 V / 1000 V peak (7993 Hz sine) | 2 / 4 / 5 / 7 | under 12 |
| Hot squares (0, +12, +24 dBFS at +36 dB, every quality, stereo, auto level off) | output within the stage's range (≤ 6.1 FS, bar 8.1); worst block 39-206 µs | the 2.67 ms period |

The waveform error grows with frequency and drive and falls with the
oversampling rate: it is the trapezoidal rule's frequency warping of the
Miller pole and of the coupling network at high harmonics, which ngspice
integrates with a much finer step. In the audio band the levels agree
within a tenth of a dB everywhere.

### Aliasing (plot: `docs/images/dsp/tube-aliasing.png`)

Non-harmonic energy from 20 Hz to 20 kHz on tones at 997, 2503, 5011,
7993, 10007, 14009 and 19001 Hz (no alias lands on a harmonic).

**"Nominal drive" is defined here** as the default drive (+6 dB) with a
-18 dBFS tone, the EBU R68 alignment level: 0.25 V peak at the stage, H2
-39 dBc. ADR-0011 §3.2 does not define it; this definition is a reviewed
choice in `quality.json`. Hotter levels are reported beside it.

| Quality, worst over the tones | 44.1 kHz | 48 kHz | 96 kHz | Bar |
|---|---|---|---|---|
| eco (2x), nominal | -88.1 dBc | -88.4 | -150.2 | -70 dBc |
| standard (4x), nominal | -145.1 | -146.5 | -150.4 | -90 dBc |
| high (8x), nominal | -145.1 | -146.3 | -150.3 | -90 dBc |
| eco, default drive at -6 dBFS (1 V), reported | -49.8 | -49.8 | -97.8 | — |
| standard, the same, reported | -89.9 | -97.8 | -150.6 | — |
| high, the same, reported | -134.5 | -132.1 | -150.6 | — |
| standard, +18 dB drive at -6 dBFS (4 V), reported | -35.5 | -38.8 | -59.4 | — |
| high, the same, reported | -55.0 | -59.4 | -76.6 | — |

The worst case is always the highest tone: at 2x and 44.1 kHz the fourth
harmonic of 19 kHz folds to 12.2 kHz. A saturated stage is not
band-limited at any of these factors; 8x keeps it under -55 dBc.

### Hot input (the RT audit of PR #52, high 1 and 2)

At 8ab60d5 an input above 0 dBFS at full drive made the solve diverge:
- it hit its 12-iteration cap and accepted the unconverged result;
- the capacitor histories ran away (5e37 V at the load);
- the plugin output went to 1e34, or silence once a non-finite block
  reset it;
- blocks took 6.5-7.3 ms at 4x (the RT audit), against the 2.67 ms period.

What fixed it:
- **The input is bounded to ±100 V** at the stage (`kMaxInputV`). 0 dBFS
  at +36 dB is 63 V. Past the bound the grid sits in grid current or
  cut-off whatever the input does, and the Dempwolf law is outside the
  range its paper fitted: DAFx-11 §6 says it is wrong for Vg > 0 with
  Va < 20 V.
- **No step damping.** The 50 V step bound stalled the solve. The plate
  legitimately moves hundreds of volts on a hot edge, and with 20 V or
  50 V bounds a 63 V square at 2x went 12 iterations unconverged. Without
  them it converges in at most 7. The bound is now 1000 V, a guard
  against an absurd Jacobian only.
- **A sample that does not converge resets.** So does one that lands
  outside |Vgk| ≤ B+, -B+ ≤ Vpk ≤ 2 B+. The stage restarts from its
  operating point, outputs 0 V for that sample, and counts it
  (`resets()`). No hot test triggers it; it stops a runaway rather than
  hides one.
- **Exact tails past the tables.** Outside the tables each law is one
  `exp` or `pow`, exact to the last bit there. Deep cut-off (Cx < -100)
  is 0, below anything the sums can see. The grid table reaches 12 V. A
  hot input therefore costs no transcendental call in steady state.
- **The table index is clamped to the last interval.** This is the code
  review's low 2: x just under `hi` rounded to the last node.

The regression test was written against the old behaviour (bounds and
reset reverted in a copy). It fails there, on a non-finite output and on
12 iterations, and passes now (`test_triode`, `test_hot_square_every_factor`):

- **Input:** squares of 63, 250 and 1000 V through the oversampler at 2x,
  4x and 8x.
- **Iterations and resets:** at most 6 iterations, 0 resets.
- **Load voltage:** within 343 V.
- **Block time:** the worst block is 1.1-2.0x the median block of a quiet
  square on the same build. The bar is 6x, a ratio so the test holds under
  the sanitizers. Before the fix it was over 100x.

The tube suite does the same at the processor
(`hot_input`). The worst Release blocks there are 39-46 µs at eco, 88-114
µs at standard and 110-206 µs at high, each inside the period. The output
peak is 6.1 FS with auto level off; the bar is the stage's range, 8.1 FS.

### CPU (§3.5, class B: p99.9 60 µs, max 150 µs)

All figures are q128, 48 kHz, stereo, single thread, 60 s, the per-block
minimum of 5 runs, on a quiet host. The runs were gated on a 1-minute
load average under 2.5; other sessions loaded this machine heavily
between them. Three loads were measured:
- the golden settings (drive +14 dB) on the harness's noise-and-sweep
  programme;
- the programme raised 12 dB at +36 dB of drive;
- a 0 dBFS 997 Hz square at +36 dB of drive.

Eco, before and after:

| Eco (2x), p99.9 / max | 8ab60d5 | Now, two runs |
|---|---|---|
| programme, drive +14 dB | 47-64 / 67-79 µs | 44.5-48.3 / 45.5-57.0 µs |
| programme +12 dB, drive +36 dB | (diverged at +12 dBFS peaks) | 47.3-48.0 / 49.0-49.9 µs |
| 0 dBFS square, drive +36 dB | 459 µs worst block, mean 356 µs (probe) | 40.0-46.1 / 47.0-53.0 µs |

Standard and high, now (reported, not held to the bar):

| Quality, p99.9 | programme | +12 dB, +36 dB | square, +36 dB |
|---|---|---|---|
| standard (4x) | 95.8-110.0 µs | 86.2-95.7 | 71.6-72.2 |
| high (8x) | 143.4-145.6 | 152.9-154.6 | 137.4-140.9 |

- **Eco is held to class B on all three loads.** It now meets the bar with
  about 20 % to spare: 48.3 µs at worst, against 47-64 µs, two runs
  missing, before. Standard and high are over class B and reported.
  ADR-0011 §3.5 lets them ship beside an eco that meets it; the class
  figures are not changed.
- **Where the time went:** perf, on cycles and instructions.
  - The solve is about 75 % of a block, the oversampler under 15 %.
  - A sample is one chain of dependent operations: last sample's
    histories, the port voltages, the predictor, the law, the Newton
    steps, the new histories. The block's cost follows the chain and
    the number of Newton passes, not the multiply-adds. Shrinking the
    linear part alone (the two capacitors) moved instructions little.
  - An eco block went from about 211k cycles on the hot square to about
    183k.
- **What did it:**
  - The two-channel solve shares one set of matrices. It runs as one
    template over a lane type (`src/triode_solve.h`): SSE2 on x86-64
    (`src/arch/x86_64/triode_pair.cpp`), two doubles elsewhere
    (`src/arch/generic/triode_pair.cpp`). Both give the bits `tick()`
    gives. `test_triode` checks that, and `dsp_triode_generic` runs the
    portable lane type on x86 too. Lane loads are two 8-byte loads: a
    16-byte load of two 8-byte stores cannot be store-forwarded and
    stalled every pass.
  - Tree sums instead of running sums.
  - One division per pass, by Cramer's rule.
  - The step bound off the common path.
  - Exact tails without transcendental calls in cut-off.
  - A predictor that changes linearisation across regimes. From cut-off
    or from grid current the last Jacobian predicts badly, so a fixed
    linearisation at the operating point, or at a point in grid current,
    predicts instead. On full-drive programme, samples taking 5 or more
    passes fell from 6.2 % to 1.2 %, and the mean from 1.52 to 1.34
    passes.
- **Tried and dropped:** a chord step reusing the first pass's Jacobian
  (it reset on hot edges), and a quadratic-extrapolation predictor (more
  passes).
- **Silence at rest is rest.** A stage that has seen only 0 since
  `reset()` keeps its state to the bit. The operating point is the
  solve's own only to its tolerance, so a host's preroll used to move the
  state in its last bits. The re-activation render then differed by 3e-8;
  now it is exact.
- **The gate** is its own section below.

### The gate (merge gate of PR #52, follow-up)

**Eco has margin; the 59.5 µs was host load.** The merge gate measured
the eco square at p99.9 59.5 µs against the 60 µs bar while other
sessions' containers ran. Re-measured on a quiet host (no other
container; the 1-minute load average under 1.5 inside the Docker VM
before each run), three 60 s runs, 2026-10-14, at `a55df5e` plus this
follow-up (which does not touch the solve's arithmetic):

| Eco (2x), p99.9 / max, µs | run 1 | run 2 | run 3 |
|---|---|---|---|
| programme, drive +14 dB | 41.2 / 41.7 | 41.3 / 41.9 | 42.3 / 43.2 |
| programme +12 dB, drive +36 dB | 44.2 / 45.4 | 44.3 / 46.1 | 45.8 / 47.3 |
| 0 dBFS square, drive +36 dB | 36.8 / 37.9 | 36.6 / 37.4 | 37.8 / 38.2 |

The worst case is 45.8 µs, 24 % under the bar and under the 50 µs the
follow-up asked for, so the solve was not optimised further. The
per-block minimum of five runs removes preemption, not a neighbour
that slows every block (a shared core, a lower clock under load): the
figure that counts is one taken on a quiet host.

**PR CI does not measure CPU** (ADR-0011 Amendment 1 item 4). The
nightly `cpu` job holds eco to class B on a shared GitHub runner, and an
absolute bar there would gate the runner's speed, not the code: the
first run of this follow-up landed on an AMD EPYC 7763, where the
unchanged eco solve measured 65.6 µs on the +12 dB programme, red
against 60, with 45.8 µs at worst on the reference machine.

So the nightly's bar is **runner-calibrated**
(`tools/dsp-cpu.sh --calibrated`, `sonora-dsp-measure --cpu-calibrate`):
- The same step first times a **reference kernel**
  (`plugins/native/measure/src/calibrate.cpp`): two channels of 256
  samples per q128 block, each a serial chain of a one-pole filter,
  three fixed Newton passes on a diode clipper (one `exp` and one
  division each), and a biquad. That is the shape of the triode solve
  (a dependent chain with transcendental calls and divisions on it, as
  perf showed), so the two scale alike across machines. Fixed iteration
  count and input: its cost does not depend on data. It uses nothing
  from `common/dsp`, so a DSP change cannot move it.
- Its p50 (per-block minimum of 5 runs, 20 s) over its p50 on the
  reference machine, `kReferenceKernelUs` = 32.6 µs (i7-11800H, the
  sonora-build:trixie container, GCC 14; 32.58-32.60 µs over three
  runs, `sonora-dsp-measure --cpu-reference-kernel`), is the runner's
  **scale**. Every CPU bar is multiplied by it; the kernel, the scale and
  the scaled bar are in the report.
- The kernel is frozen: an edit to it, its flags or its timing must
  re-measure `kReferenceKernelUs` on the reference machine in the same
  commit.
- Locally, and on the reference machine, `tools/dsp-cpu.sh` without the
  flag holds the absolute bar, as before.

Measured on GitHub runners, the nightly dispatched on this branch:

| Runner | kernel | scale | eco p99.9 worst (bar) | uncalibrated (bar 60) |
|---|---|---|---|---|
| AMD EPYC 7763, run 38505906342 | 42.1 µs | 1.291 | 64.4 µs, +12 dB programme (77.5) | 65.6 µs: red |
| AMD EPYC 9V45, run 38506494424 | 29.7 µs | 0.911 | 42.8 µs, +12 dB programme (54.6) | 42.9 µs |

In kernels, eco's worst case costs 1.40 on the reference machine, 1.44
on the EPYC 9V45 and 1.53 on the EPYC 7763: the kernel tracks the solve
across three microarchitectures to within 10 %. The calibrated bar is
60 / 32.6 = 1.84 kernels, so the slowest-tracking runner still has 20 %
headroom (the reference machine 31 %), and the gate is green on any of
them with the code as it is, faster runner or slower. It goes red when
eco gets about 20-30 % slower, by the runner's draw; anything smaller is
for the reference-machine run of `tools/dsp-cpu.sh`, which holds the
absolute bar, to find before a figure is recorded here.

**Diagnostics kept across a restart** (merge gate, low). A failed sample
restarted the stage through `reset()`, which also cleared
`max_iterations_seen()`, so the count forgot the failure `resets()`
recorded (8 kHz, a ±100 V square: 714 resets, 7 iterations at most).
The restart is now `restore_operating_point()`, the same bounded copy
without the counts; `reset()` stays the caller's and still clears them.
`test_triode`'s `test_failure_keeps_iteration_count` reads 12 now.

### Bundles (`tools/build-plugins.sh`)

- `sonora-tube.lv2`, `sonora-tube-mono.lv2`, `.vst3` of each: lv2lint
  0.16.2 passes both; the VST3 validator 3.8.1 passes 47 of 47 for both.
- 14 renders through `sonora-plugin-host`, LV2 and VST3 (golden at 44.1,
  48, 96 kHz, bypass, mono, automation, and a quality change taken at a
  re-activation), all with max |plugin - reference| = 0, and the
  reported latency equal to the reference's.
- The reference after the host's 512-sample preroll differs from the
  golden file by 2e-14 at most: the float rounding of the rest output,
  not the state, which silence at rest no longer moves.
- ASan and UBSan: `dsp_triode`, `dsp_triode_generic`, `native_tube`, and
  the bundle driver tests, clean. TSan: `native_format_*_tube*`, clean.

## Not measured, and why

- **Plate curves against the datasheet.** The laws and parameters are
  the papers' own fits to measured 12AX7s; this PR checks the circuit
  solver against SPICE on the same laws, not the laws against a tube. A
  comparison with published curves (or the owner's tubes) is in BACKLOG.
- **MSVC and Clang golden runs.** The project has no MSVC CI job yet and
  the build container has GCC only (`native.rtsan` is open for the same
  reason). The arithmetic avoids FMA contraction (`-ffp-contract=off`)
  and uses only `std::exp`, `log1p` and `pow` at prepare and outside the
  tables, so differences would be in the last bits; not yet shown.
- **Denormals on a non-x86 build.** `generic/denormals.cpp` does not flush,
  and `ProcessorBase`'s guard is the only flush, on x86-64 (MXCSR). The
  stage's own state does not decay to zero: every capacitor holds a DC
  voltage at rest, and silence at rest skips the solve. But the
  oversampler's FIR histories and the dry delay do decay to zero. On an
  arm64 build they reach subnormals after about 16 s of silence. A flush
  for arm64 (FPCR.FZ) in `src/arch/aarch64/` is a BACKLOG item before the
  first ARM build.
- **RTSan.** No Clang in the build image (`native.rtsan`). `process()`
  allocates nothing (tables, oversampler buffers and the dry delay are
  sized in `prepare()`); the rt-safety-auditor's review is the check
  until the RTSan job exists.
