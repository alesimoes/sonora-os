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
across three microarchitectures to within 10 %. The scaled class bar is
60 / 32.6 = 1.84 kernels, so on its own it trips on a regression of about
20 % on a 7763 and 28 % on a 9V45 (review of PR #55): which one depends
on the runner the job draws. Two additions make the gate the same on
every runner:
- **A baseline in kernels.** `quality.json` records `cpu.p999_kernels`
  = 1.53, eco's worst p99.9 on the slowest-tracking machine measured
  (the EPYC 7763, +12 dB programme), and each eco load's p99.9 in
  kernels is held to it x 1.15 = 1.76. That trips on a regression of
  about 15 % on any of the three machines; the scaled class bar stays as
  the backstop. A deliberate change to the solve's cost re-records the
  baseline from a nightly's report, in the same commit, and says why.
- **The kernel is timed before and after the loads.** A neighbour that
  slows every block, or a clock step, while the loads run is something
  the per-block minimum cannot remove, and a kernel timed in another
  phase would mis-scale the bar both ways (a false red, a hidden
  regression). The harness times the kernel before the first load and
  again at the end; when the two differ by more than 5 % the calibrated
  checks are **inconclusive**: reported without a bar, neither passed
  nor failed, `sonora-dsp-measure` exits 3, `tools/dsp-cpu.sh` prints
  INCONCLUSIVE and exits 3, and the nightly step turns that into a
  warning annotation, not a red and not a pass. Otherwise the scale is
  the mean of the two. On a quiet reference machine the two timings
  agree to 0.03 % (32.579 and 32.588 µs); with the load average at 3.4
  (other sessions' containers) they differed by 6.9 % (34.87 and 32.60 µs),
  and that run came out inconclusive, as it should.
- The max bar is scaled too. A p50 ratio has no physical claim on a
  tail's maximum, but the 150 µs bar is lenient enough that it adds no
  flake; it is reported, and the p99.9 checks are the gate.

Quiet reference machine, calibrated, after these changes: eco p99.9
41.4 / 45.0 / 37.1 µs (programme, +12 dB programme, square), 1.27 /
1.38 / 1.14 kernels against 1.76; absolute, the same build, with the
load average at 3.4-3.7 (another session's container): 44.7 / 47.2
/ 40.3 µs against 60.

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

## Slice 3: the output stage (transformer and supply sag)

`common/dsp/transformer.h`, `OutputStage`; in the tube drive as
`transformer` (0 % bypasses it exactly, the default) and `sag`.

**The circuit.** A push-pull driver (a voltage behind Rs = 1.5 k) feeds
the magnetising branch of an output transformer in parallel with the load
(3 k, referred). The node equation is `v = g Vs - Rp i` with
`d psi / dt = v`, integrated with the trapezoidal rule at the oversampled
rate; one scalar equation per sample, strictly monotone in `i`, so a
safeguarded Newton converges in at most 4 iterations over the cases
measured (1e-6 to 1000 of full scale, sines and squares; bound 24). The
driver cannot swing past the supply rail: `Vs = x / (1 + (x/rail)^4)^(1/4)`.
That curve is ours, a smooth stand-in for the load line of a class-AB
pair; it is not from a paper and is not validated against a circuit. The
supply is a 100 uF reservoir behind 600 ohm (rectifier and winding) at
Sag 100 %, feeding 60 mA and the rectified primary current: it sags with
the signal and recovers with tau = 60 ms.

**The core** is Jiles-Atherton (Jiles and Atherton 1986; the update and
its guard for the non-physical region after Jiles, Thoelke and Devine
1992), written in amperes with `psi = Lc (i + M)`. The step is
explicit in the irreversible magnetisation and clamped so it never passes
the anhysteretic curve it chases; that makes `psi(i)` monotone, which the
bracketing relies on. The scheme is first order in the hysteresis update:
60 Hz at half scale differs 1.2e-4 (96 vs 192 kHz) and 6e-5 (192 vs
384 kHz) of the peak.

**Parameters are ours.** Lc = 10 mH, Ms = 80 A (saturation flux 0.8 V*s),
a = 20 mA, k = 6 mA, c = 0.3, alpha = 2e-5; 1.0 of signal = 250 V at the
driver, rail 271 V at idle. They are in the range of a small audio output
transformer, set so the low-field corner is near 40 Hz (measured -2.9 dB
at 40 Hz and 1 mV of drive, -6.9 dB at 20 Hz). They are not fitted to any
unit and not checked against a published measurement.

**Measured** (`test_dsp_transformer`, `native_tube`; GCC, x86-64, a busy
Docker Desktop host):
- Bass first: at half scale the THD is 0.49 % at 1 kHz, 1.4 % at 80 Hz and
  2.3 % at 40 Hz; the flux at a fixed level goes as 1/f (200 Hz over
  400 Hz: 1.90 for an ideal 2).
- Hysteresis dissipates: the loop integral of `i d psi` over a cycle at
  40 Hz is positive and falls 8.5x when the coercivity falls 10x.
- Through the processor (drive 0 dB, auto level): the stage adds -58 dB of
  distortion (harmonics 2-9 against the fundamental) at 1 kHz and -31 dB
  at 40 Hz, at -12 dBFS: 27 dB bass-first.
- Sag: a full-scale 100 Hz burst pulls the rail 14 % down (271 to 232 V)
  and it returns to within 0.1 V in 600 ms. A held -1 dBFS 100 Hz tone
  ends 0.098 dB below where it began at Sag 100 % and level at Sag 0 %.
  The effect is small at moderate levels because the rail has headroom;
  it grows as the level rises towards the rail.
- Aliasing with the stage in, nominal level (-18 dBFS), 48 kHz: -88 dBc
  eco, -113 dBc standard, -123 dBc high (bars -70, -90, -90). At drive
  +18 dB and -6 dBFS (reported) the preamp's own overload dominates.
- Latency unchanged with the stage in (0 samples measured - reported at
  44.1, 48 and 96 kHz, eco and high); the zipper bar holds for
  `transformer` and `sag`.

**Not done.** No SPICE reference for the core (ngspice's core model is a
Jiles-Atherton and is the natural reference; the build image has no
ngspice); no golden for the stage on (the golden stays the stage off); the
CPU cost of the stage on is reported below, not held.

**CPU of the stage on** (`--cpu`, q128 stereo 48 kHz, GCC Release, a busy
Docker Desktop host on which the stage-off eco p50 read 98 us, so read the
ratios): eco p50 281 us / p99.9 475 us with Transformer 100 % on the
programme; standard p50 506 us, high p50 459 us. The stage costs roughly
two to three times the preamp (a Newton solve with a tanh per iteration for
the Langevin function, per oversampled sample, per channel). It is over
class B and so is reported, not held; the defaults (stage off) still meet
the bar. Reducing it is a follow-up: a tabulated Langevin, a warm start
from the previous step's slope, and running the stage at 2x whatever the
quality.

## Output stage: finite for any input, and a cheaper solve

- `OutputStage::tick` caps |u| at 1e6 before u^4. Past that the soft clip is
  flat at the rail to 1e-6 relative. Before (corrected in the follow-up
  below; this line first said x = 1e300 gave NaN): from |x| of about 1.3e77
  u^4 overflowed to infinity and vs = rail u / inf = 0, finite but silent,
  so x = 1e100 or 1e300 dropped the output to 0 instead of holding it at
  the rail; vs was NaN (inf / inf) only above about 7.2e305, where x * vpu
  itself overflowed. Regression test: `test_dsp_transformer` extremes,
  squares of +-1e100, +-1e300 and 1.7e308 keep the output, the rail and the
  core finite, hold the output to within 1e-6 of the rail's level of what a
  square that merely clips (1000 of full scale) gives, and leave the stage
  usable afterwards.
- The solve is cheaper: one exp serves the Langevin function and its slope
  (was tanh and sinh), and the Newton start uses the previous step's
  stored slope and psi instead of an extra evaluation.
- Measured (`tools/dsp-cpu.sh`, i7-11800H in the Docker image, a host
  running 2.58x slower than the recorded reference kernel, so read ratios;
  eco, stage 100 %): p50 274 us against 120 us stage off, 2.3x. The note
  above read 281 against 98, 2.9x, on a busy host. Divided by the runner
  scale, eco with the stage on is about 106 us against the 60 us bar.

**Class B is not met** by the stage on at eco (about 1.8x over), nor by
standard (4x: p50 247 us stage off, 549 us on) or high (8x: 464 / 1016 us)
on this host (divide by 2.58 for the reference machine). Those need a
different structure, not tuning: a tabulated core running at the base
rate in eco is the next candidate, and needs its own SPICE reference
for the core. ADR-0011 section 3.5 lets standard and high ship over the bar
beside an eco that meets it; with the stage off eco still does (150.6 us
against 154.5 us calibrated bar, 1.79 kernels against 1.76 in this run:
host noise, not a change, as the stage is not in that path). Corrected in
the follow-up below: that run failed its kernel gate, so it did not show
eco meeting class B; the rerun there does.

## Output stage at class B (follow-up of #219)

**What changed** (`transformer.h`, `src/transformer_solve.h`,
`src/arch/*/transformer_pair.cpp`):
- **The core steps at the base rate in eco**, the driver's clip stays
  oversampled. `OutputStage::prepare(..., substeps)`: the clip runs on
  every sample, the core once per `substeps`, with its current taken
  linear across the step and the driver's flux summed sample by sample
  (with one substep, the plain trapezoid as before). The magnetising
  current is the integral of the node voltage through the core, low-passed
  (low-field corner near 40 Hz), so it does not need the oversampled rate.
  Eco steps the core at 48 kHz (2 substeps); standard and high at 96 kHz
  (2 and 4), because at 48 kHz their aliasing with the stage in read
  -92.1 and -91.8 dBc, too close to their -90 bar; at 96 kHz it reads -97.7
  and -97.2 (it was -113 and -123 with the core oversampled). Eco reads
  -87.7 dBc against -70, as before (-88): its floor is the oversampler's.
  Against the core at the full rate, 0.5 FS sines at 40 Hz and 1 kHz
  differ by 2.0e-4 to 4.0e-4 of full scale (-68 to -62 dB) and keep their
  THD (2.31 % and 0.49 %) and gain.
- **One evaluation per step.** The Newton step left after the first
  evaluation is taken along the slope just evaluated instead of
  evaluating again, and the solve stops when that step stands for under
  1e-4 of full scale at the output. Against the same solve converged to
  -200 dB the output differs by -94 dB of its peak at worst (programme
  12 dB into the rail, 48 kHz), 25 dB under what stepping the core at 48
  rather than 96 kHz changes; 1.000 to 1.088 evaluations per step at
  48 kHz (2.04 to 2.76 converged). `test_dsp_transformer` holds both
  (-90 dB, and 1.02 and 1.15 evaluations per step) so the cost cannot creep
  back on any machine.
- **The Langevin function from a table**: cubic Hermite on L and L' at
  641 nodes over [0, 20] (h = 1/32), exact at the nodes, C1, its slope the
  interpolant's own; error under 1e-9, about 1e-12 of full scale at the
  output; 1 - 1/x above 20 (exact in doubles). It replaced an exp and a
  division on the chain of dependent operations.
- **The two channels as the two lanes of one solve**, as the triode does:
  the template in `src/transformer_solve.h`, SSE2 on x86-64, two doubles
  elsewhere, the hysteresis branches as selects; a lane the first
  evaluation does not settle takes the scalar loop. Bit-identical to the
  one-channel solve on both lane types (`test_dsp_transformer`, "two
  channels in lockstep"), and the processor runs a block per call
  (`process_block`), identical to step-by-step calls with the sag moving.
- Fewer divisions in the clip and the supply (reciprocals at `prepare`
  and `set_sag`, which now returns early when the sag has not moved).

**Measured** (i7-11800H, Docker Desktop, GCC 14 Release; the host's speed
moved by up to 2.4x between runs, so the figures are the stage-on against
stage-off ratio inside one process, renders interleaved, per-block minimum
of 5-7 runs of 5 s, q128 48 kHz stereo; the scratch harness renders the
suite's CPU loads):

| p99.9 on/off | origin/main | this change |
|---|---|---|
| eco, programme | 2.64 | 1.34 |
| eco, programme +12 dB, drive +36 dB | 2.05 | 1.31 |
| eco, 0 dBFS square, drive +36 dB | 2.05 | 1.37 |
| standard, programme / hot / square | 2.00 / 2.49 / 1.73 | 1.33 / 1.33 / 1.42 |
| high, programme / hot / square | 2.34 / 2.04 / 2.21 | 1.23 / 1.21 / 1.22 |

At eco the stage adds 35.4-36.7 us of host time per block where it added
99-174 us: 2.8x to 4.7x cheaper. On a run where the host tracked the
reference kernel (scale 2.56, stage-off eco 1.28 kernels as on the quiet
reference machine), that is about 14 us on the reference machine. Added to
the recorded quiet-reference stage-off figures (The gate): programme 41.4
+ 14 = 55 us, square 37.1 + 14 = 51 us, hot programme 45.0 + 14 = 59 us,
against 60 us; the hot programme is 1.81 kernels, over the 1.76 the gate
holds eco to (cpu.p999_kernels x 1.15) and near the 1.84 kernels of the
class bar itself, and the EPYC 7763 runner tracks about 9 % worse.

**So class B is not claimed for the stage on.** It is within the bar on
the projection but without margin on the hot programme, the projection is
not a measurement on the reference machine, and holding it would turn the
nightly red on the slowest runner. The stage-on loads stay reported, not
held (now with the hot programme too); the default (stage off) eco is the
mode that meets the class, as ADR-0011 section 3.5 allows. What is left: an
instruction count of about 660 per stereo step at eco (callgrind), spread
over the step (no line over 3 %); the next candidates are the state kept in
lane layout across a block instead of reloaded per step, and the clip's two
square roots. Then a quiet reference-machine run decides.

**Eco with the stage off, rerun** (item (d)): `sonora-dsp-measure --cpu
--cpu-calibrate --cpu-seconds 10`, scale 2.558, kernel before/after 0.33 %
apart: p99.9 106.99 us (bar 153.5), 1.28 kernels; +12 dB programme 1.39;
square 1.14; noise and hum 1.35; all against 1.76: eco meets class B with
the stage off. A run on the same build at a load average of 19 read 1.80 to
2.20 kernels and failed, as did origin/main's build in the run beside it
(1.67 to 1.93): host load, not the code, which this change leaves
bit-identical with the stage off (the golden holds).

### The rerun: the step's chain of dependent operations

The step is latency-bound, not instruction-bound: one solve per core step,
each needing the last one's state, and the supply's rail feeding the next
step's clip. What changed (`src/transformer_solve.h`, `transformer.h`,
`src/arch/*/f64x2.h`):
- **State in lane layout across the block**: loaded from `Core` once per
  `process_block`, stored once at its end; the constants are broadcast once
  into a local (`CoefV`) the output stores cannot alias; `eval_lanes` is
  declared `inline` (GCC had left it out of line: a call per step, every
  register spilled around it). The Langevin table is `double[641][2]`, read
  one node per 16-byte load, the index converted in vector form
  (`trunc_index`, `gather2`). All bit-identical to the per-step form.
- **The driver's rail one core step behind** (`rail_drv_`): the clip of
  step t uses the rail after step t - 2's draw, not t - 1's, and is computed
  during step t - 1, before its solve, so it runs beside the solve instead
  of after the supply. One step is 20.8 us at eco (10.4 us at 96 kHz),
  3.5e-4 of the reservoir's 60 ms (600 ohm x 100 uF). This is the one
  change of output. Against the bit-exact form (stage alone, stereo, sag
  100 %, 5 s): 0.3 FS programme -118 dB of peak; 1.2 FS (into the rail)
  -85.8 dB; standard and high -91.7 and -91.4 dB; a 4 FS square, the
  extreme, -80.4 dB; 0 with sag at 0 (bit-identical). All under the -62 to
  -68 dB that stepping the core at 48 rather than 96 kHz moves it by. The
  golden files are unchanged (the stage is out at the golden settings);
  every check of the suite and of `test_dsp_transformer` passes as before.

**Measured** (i7-11800H, Docker Desktop, GCC 14 Release, -O3
-ffp-contract=off, other agents' builds running beside it):
the stage alone, origin/main and this change in one binary, renders
interleaved, per-block minimum of 7 runs, 128 core steps per block:

| p50 per block, us | origin/main | this change | ratio |
|---|---|---|---|
| eco, 0.3 FS programme, sag 100 % | 32.99 | 18.89 | 0.573 |
| eco, 1.2 FS, sag 100 % | 33.59 | 19.61 | 0.584 |
| eco, 4 FS square, sag 100 % | 35.14 | 20.73 | 0.590 |
| standard (96 kHz core), 1.2 FS | 33.07 | 18.95 | 0.573 |
| high (96 kHz core, 4 substeps), 1.2 FS | 35.52 | 24.40 | 0.687 |
| eco, 1.2 FS, sag 0 % (bit-identical) | 28.54 | 18.24 | 0.639 |

The bit-identical part alone gave 0.875 at sag 100 % (0.76 at sag 0); the
rail's step of lag the rest. In the suite (`sonora-dsp-measure --processor
tube --cpu --cpu-calibrate --cpu-seconds 5`, runner scale 2.565, kernel
before/after 0.5 % apart), eco's stage-on minus stage-off p99.9: programme
+19.96 us, hot +22.1 us, square +20.65 us, where origin/main's build, run
beside it, read +48.9 to +49.2, +38.9 to +42.5 and +38.5 to +41.0. The
absolute figures failed for both builds with the stage off too (1.78 to
1.97 kernels against the quiet 1.28): host load, as in the run above.

**Projection, and why the stage-on loads stay reported**: +22.1 us at
scale 2.565 is 8.6 us on the reference machine (it was 14). Added to the
quiet stage-off figures: programme 41.4 + 7.8 = 49.2 us (1.51 kernels),
square 37.1 + 8.1 = 45.2 (1.39), hot programme 45.0 + 8.6 = 53.6 us, 1.64
kernels, against the 1.76 gate and 1.84 for the class bar: within both on
the reference machine, 7 % under the gate. With the EPYC 7763 runner's
9 % (above), the hot programme reads 1.79 kernels, 0.03 over the gate
(about 1 us per block on the reference machine) though under the class bar
(1.84). The gate is the stage-off eco's recorded worst case plus 15 %; a
stage-on load needs its own recorded worst case, which takes a nightly run
on that runner (the stage-on figures are in its report). So the stage-on
loads stay reported, not held, until a nightly records them; then they are
held to the class bar and to their own recorded figure.
