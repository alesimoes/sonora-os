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
| Interelectrode capacitances | The 12AX7 datasheet values both papers quote: Cgk 2.3 pF, Cgp 2.4 pF, Cpk 0.9 pF | In the stage |
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

## The harness

- The netlist is generated from the same `TriodeStageCircuit` and
  `TriodeDevice` values the model is built from, the laws as ngspice
  B-sources. The generated netlists are committed; `spice_triode_*`
  (ctest label `spice`, only where ngspice is installed) checks that they
  are byte-identical to what the model's values generate, re-runs
  ngspice, and checks the harmonics against the committed
  `reference.json` to 1e-6 of the fundamental and the operating point to
  1e-6 V. Each of the 16 cases is its own ctest; 6 minutes of CPU, 2 in
  parallel.
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
| Fundamental vs ngspice, 16 cases at 176.4, 192 and 384 kHz | ≤ 0.0037 dB | 0.05 dB |
| Harmonic levels vs ngspice, every harmonic ≤ 20 kHz and ≥ -80 dBc | ≤ 0.093 dB (Koren, 4 V, at 192 kHz); Dempwolf ≤ 0.05 dB | 0.5 dB |
| Waveform error to 20 kHz, \|model - SPICE\| / \|H1\|, complex | ≤ -52.9 dB (Dempwolf, 4 V, 5 kHz, at 176.4 kHz); -66.5 dB at 384 kHz | -40 dB |
| The same at 88.2 kHz (eco at 44.1 kHz), reported | -40.7 dB | — |
| Processor through the oversampler, harmonic levels vs ngspice, 12 Dempwolf cases at 44.1, 48, 96 kHz | ≤ 0.054 dB | 0.5 dB |
| Tabulated law against the exact one, through the stage | -180 dB | -120 dB |
| Small-signal gain at 1 kHz, drive 0 dB, auto level | within 1.5e-5 dB of unity | 0.05 dB |
| Small-signal response at 20 Hz, 100 Hz, 10 kHz, 20 kHz (the coupling capacitors and the Miller pole), reported | -1.74, -0.07, -0.21, -0.82 dB at 48 kHz | — |
| Latency, measured - reported, eco/standard/high at 44.1/48/96 kHz | 0 (69/39/11, 76/45/16, 78/47/18 samples) | exact |
| Bypass = input delayed by the latency | exact | exact |
| Non-finite and 1e30 input | finite; identical to silence in their place from the next block | §3.2 |
| Denormals: silence after signal / active | 0.56 | 1.2 |
| Zipper, full-range sweeps of drive, output, mix, auto level, -40 dBFS tone | ≤ -120.5 dBFS | -94 dBFS (-60 moved with the tone) |
| Golden renders 44.1/48/96 kHz | identical | 1e-6 |
| Newton iterations per sample, most: 1 V / 4 V / 63 V peak | 3 / 5 / 11; 12 (the cap) at 1000 V, output finite | 12 |

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
| eco (2x), nominal | -88.4 dBc | -88.7 | -150.4 | -70 dBc |
| standard (4x), nominal | -145.2 | -146.5 | -150.5 | -90 dBc |
| high (8x), nominal | -145.2 | -146.5 | -150.4 | -90 dBc |
| eco, default drive at -6 dBFS (1 V), reported | -49.8 | -49.8 | -97.8 | — |
| standard, the same, reported | -89.9 | -97.8 | -150.6 | — |
| high, the same, reported | -134.5 | -132.1 | -150.6 | — |
| standard, +18 dB drive at -6 dBFS (4 V), reported | -35.5 | -38.8 | -59.4 | — |
| high, the same, reported | -55.0 | -59.4 | -76.6 | — |

The worst case is always the highest tone: at 2x and 44.1 kHz the fourth
harmonic of 19 kHz folds to 12.2 kHz. A saturated stage is not
band-limited at any of these factors; 8x keeps it under -55 dBc.

### CPU (§3.5, class B: p99.9 60 µs, max 150 µs)

Golden settings (drive +14 dB) on the harness's noise-and-sweep signal,
q128, 48 kHz, stereo, single thread, 60 s, per-block minimum of 5 runs:

| Quality | p50 | p99.9 | max |
|---|---|---|---|
| eco (2x) | 44-50 µs | 47-64 µs over four runs | 67-79 µs |
| standard (4x) | 82-89 µs | 95-104 µs | 116-122 µs |
| high (8x) | 151-155 µs | 173 µs | 189-197 µs |

- Standard and high are over class B. ADR-0011 §3.5: "a processor over
  its class budget ships only with an eco mode that meets it." The suite
  holds eco to the bar and reports the other two.
- **Eco is at the bar, not under it with margin:** p99.9 read 47, 53,
  62 and 64 µs in four runs of 10 to 60 s on this machine, so two of
  four runs miss 60 µs. The p50 is 44-50 µs. In PR CI the figure is not asserted
  (Amendment 1 item 4); the reference-machine run decides.
- Where the time goes (perf, Release): about 70 % the Newton solve, 17 %
  the oversampler. A stage sample costs about 40 ns per channel when two
  channels are solved in lockstep (`TriodeStage::tick2`), which overlaps
  two independent dependency chains; one channel alone costs more.
- Ways to recover it, not done here (BACKLOG): SIMD for the lockstep
  solve in `common/dsp/src/arch/`; a chord (fixed-Jacobian) Newton step;
  the DK method's precomputed solution table over the two port voltages
  (Yeh 2010), traded against accuracy to ngspice; leaving out Cgk and Cpk,
  whose poles are above 3 MHz (measured in ngspice: ≤ 0.01 dB in band,
  0.16 dB at the 8th harmonic of 5 kHz).

### Bundles (`tools/build-plugins.sh`)

- `sonora-tube.lv2`, `sonora-tube-mono.lv2`, `.vst3` of each: lv2lint
  0.16.2 passes both; the VST3 validator 3.8.1 passes 47 of 47 for both.
- 14 renders through `sonora-plugin-host`, LV2 and VST3 (golden at 44.1,
  48, 96 kHz, bypass, mono, automation, and a quality change taken at a
  re-activation), all with max |plugin - reference| = 0, and the
  reported latency equal to the reference's.
- The reference after the host's 512-sample preroll differs from the
  golden file by 1.5e-8: the stage's rest state is a Newton solution, so
  silence leaves about 1e-8 V at the load (-210 dBFS after the
  normalisation). Under the golden bar, and asserted by the fixture.
- ASan and UBSan: `dsp_triode`, `native_tube`, and the bundle driver
  tests, clean. TSan: `native_format_*_tube*`, clean.

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
- **RTSan.** No Clang in the build image (`native.rtsan`). `process()`
  allocates nothing (tables, oversampler buffers and the dry delay are
  sized in `prepare()`); the rt-safety-auditor's review is the check
  until the RTSan job exists.
