# ADR-0152 - Class E measured with the cabinet IR included

- Status: Accepted (measurement; the quiet-machine rerun is open, see Unverified)
- Date: 2026-11-12
- Item: `ampcab.budget` (the 200 ms IR half; the WaveNet half was measured in #318, LSTM and AVX2 in #602 and #342)
- Relates to: ADR-0011 §3.5 (class E), §4.3 (cabinet IR), Unverified 9; ADR-0111 (LSTM kernel)
- Wire: no protocol change, no format change, no code change outside `plugins/native/ampcab/tools/budget.cpp`.

## Summary

The class-E budget tool now times each amp network followed by a 200 ms cabinet
IR (9600 taps, partitioned convolution, quantum-sized partitions). Against
class E (p99.9 <= 250 us, max <= 500 us) on the measured machine, the trainer's
standard WaveNet fits only with the SIMD table, the lite WaveNet and the 1x24
LSTM need it too, and the LSTMs above 2x32 and 1x128 do not fit on either table.
The standard-size network that needs the eco mode of ADR-0011 §3.5 is the
standard WaveNet (16 channels, 13 802 weights) on generic kernels.

## Context

ADR-0011 §3.5 sets class E for one amp capture plus its cabinet IR, and Unverified
9 asked for the per-block cost of the common architectures with a 200 ms IR,
measured before the first capture session. The tool `ampcab-budget` measured the
amp networks alone; its header claimed the IR was included, but the timed loop
did not run the convolver. This ADR closes that gap and records the figures and
the verdict. The verdict decides what the capture procedure (N3) may capture at.

## Decision

1. `ampcab-budget` times, per block, the amp network and then the cabinet's
   convolution (`sonora::dsp::Convolver`, `ConvolutionKernel`), 200 ms at 48 kHz
   (9600 taps), partition 128 = the quantum. The IR is a deterministic decaying
   noise, 60 dB down at its end: the convolver's arithmetic does not depend on
   its taps. The convolver is reset before each timed run.
2. The class-E verdict per architecture is the table below, applied to the
   measured p99.9 and max *with the IR*. A row fits when both figures are within
   class E.
3. The verdict is a measurement on the owner's kind of machine (see Measurement
   conditions), not a CI gate. Timing is not asserted in any test (ADR-0011 §3.5
   and the `timing` label rules).

## Measurement conditions

- Machine: Intel Core i7-11800H (8 cores, 16 threads), Windows 11 Enterprise
  host, Docker Desktop, `sonora-build:trixie` container, CMake Release, the
  container's compiler. Run by `tools/build-slot.sh` (the machine lock), one run,
  default 20 s of audio per size (7500 blocks after 200 untimed).
- **Not a quiet machine.** It is a Windows host running Docker Desktop and other
  agents' worktrees; the lock serialises heavy jobs, not every process. The
  figures are an upper bound on what a quiet host shows.
- Random weights (the arithmetic does not depend on them). `avx2` is the SIMD
  table on this CPU; `generic` is the portable path.

## Output (one run, pasted from the container)

```
WaveNet per block at q128, 48000 Hz, mono; 7500 blocks (20 s) each
size      kernels   weights   p50 us   p99.9 us   max us   RT load   CPU us
nano      generic       842     46.5      125.0    176.5      1.7%     47.4
nano      avx2          842     22.3       97.6    132.5      0.8%     23.4
feather   generic      3026     97.9      223.4    277.0      3.7%    105.5
feather   avx2         3026     39.4      124.7    240.0      1.5%     45.3
lite      generic      6554    171.7      331.0    443.5      6.4%    174.6
lite      avx2         6554     58.4      143.1    254.5      2.2%     59.5
standard  generic     13802    294.4      525.9    580.2     11.0%    299.8
standard  avx2        13802    106.3      179.4    217.6      4.0%    107.9
LSTM per block at q128, 48000 Hz, mono; 7500 blocks (20 s) each
size      kernels   weights   p50 us   p99.9 us   max us   RT load   CPU us
1x16      generic      1201     85.6      178.0    227.3      3.2%     87.2
1x16      avx2         1201     40.6       77.0     98.7      1.5%     41.4
2x8       generic       905     82.8      198.1    252.0      3.1%     84.3
2x8       avx2          905     41.1       78.7    135.2      1.5%     42.5
1x24      generic      2569    126.1      258.9    426.3      4.7%    134.3
1x24      avx2         2569     47.7       88.3    241.6      1.8%     48.7
2x32      generic     12833    348.4      476.0    611.7     13.1%    348.3
2x32      avx2        12833    131.3      189.0    343.2      4.9%    133.1
4x64      generic    116545   2125.4     4543.3   5961.0     79.7%   2294.9
4x64      avx2       116545    824.7     1048.8   1855.7     30.9%    848.6
1x128     generic     66945   1212.5     2983.4   3610.5     45.5%   1349.8
1x128     avx2        66945    632.5     2211.4   2683.6     23.7%    704.7
class E: p99.9 <= 250 us, max <= 500 us, with the cabinet IR included
```

## Verdict per architecture (class E, IR included, this machine)

| Architecture | generic | avx2 (SIMD table) |
|---|---|---|
| WaveNet nano (4 ch) | fits (125 / 177) | fits (98 / 133) |
| WaveNet feather (8 ch) | fits, p99.9 margin 27 us (223 / 277) | fits (125 / 240) |
| WaveNet lite (12 ch) | **does not fit**, p99.9 331 | fits (143 / 255) |
| WaveNet standard (16 ch, the trainer's standard) | **does not fit**, p99.9 526, max 580 | fits (179 / 218) |
| LSTM 1x16 | fits (178 / 227) | fits (77 / 99) |
| LSTM 2x8 | fits, max margin 248 us (198 / 252) | fits (79 / 135) |
| LSTM 1x24 (trainer preset) | **does not fit**, p99.9 259 | fits (88 / 242) |
| LSTM 2x32 | **does not fit**, p99.9 476, max 612 | fits (189 / 343) |
| LSTM 4x64 (largest the kernel takes) | **does not fit** (4543 / 5961) | **does not fit** (1049 / 1856) |
| LSTM 1x128 | **does not fit** (2983 / 3611) | **does not fit** (2211 / 2684) |

Reading: the SIMD table carries the trainer's standard WaveNet, lite WaveNet and
1x24 LSTM into class E on this CPU. Without it (generic kernels) the standard and
lite WaveNets and the 1x24 LSTM miss, and the eco mode a generic-only host must
ship is feather-size or smaller.

## Rationale

- The IR is part of what a capture session runs, so the budget includes it. The
  earlier amp-only rows could pass a capture that the real chain then missed.
- Per-architecture rows, not one verdict: the load check (ADR-0011 §3.5) refuses
  a capture by its architecture on this machine, so the table is what it is
  checked against.
- Both kernel tables are reported, because the shipped binary chooses the table
  at run time, and a host without AVX2 gets the generic numbers.

## Consequences

- **Which network needs the eco mode.** The standard-size network, the trainer's
  standard WaveNet (16 channels, 13 802 weights), does not fit class E on the
  generic kernels (p99.9 526 us). Its eco mode, the one that meets class E on a
  host with no SIMD table, is the feather-size WaveNet (p99.9 223 us, 27 us of
  margin, not a quiet-machine figure). The lite WaveNet also needs the SIMD table.
- The LSTMs above 2x32 (4x64, 1x128) do not fit on either table. The load check
  refuses them (ADR-0011 §3.5); no shipped capture uses them. 2x32 needs the SIMD
  table.
- Recommendation (not yet a decision of the owner): default captures use the
  architectures that fit on the generic table (nano, feather, 1x16, 2x8) until
  the quiet-machine rerun says otherwise.
- The N3 capture procedure captures at a size from this table, not from the
  arithmetic.

## Unverified

1. **The quiet-machine rerun.** Handed to the researcher: the same command on a
   quiet host (no other worktree building, Docker Desktop idle, the figures per
   machine recorded). Until then, the margins above are as measured in a busy
   Windows host, and the verdict is provisional.
2. **The IR's own share.** The IR is timed with the network, not alone, so its
   cost is not separated. A separate row is not needed for the verdict but would
   say how much of each margin the cabinet takes.
3. **Other hosts.** Only this i7-11800H and the container's compiler were used.
   Other CPUs, the MSVC build (the Portable job builds this tool) and the Linux
   appliance's kernel were not measured.
4. **The sanitizer budget.** The 500/1000 us ASan figures (#668) are not
   re-derived here; this run is Release only.
5. **Random weights.** The arithmetic does not branch on values, but a trained
   capture's denormal behaviour is not measured here (flushed by the kernels).

## Alternatives rejected

- **Gating CI on these figures.** Wall-clock timing under ASan and on shared
  runners is noisy; a timing gate is a flake (the `timing` rules). The figures
  stay a reported measurement, with their machine.
- **One pass/fail row for the whole chain.** Hides which architecture fails and
  why; the load check needs the per-architecture table.
- **Reporting the amp network alone, with the IR's cost "added later".** Adding
  the two shares of separate runs under different load is not a measurement of
  the chain.
- **Calling the standard WaveNet fit from the AVX2 row alone.** The generic path
  is what a host without the SIMD table runs; it does not fit, so the eco mode
  is needed there.
