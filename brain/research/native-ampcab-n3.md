# Amp and cabinet, stage N3 slice 1: the WaveNet kernel, the .nam reader, class E

- Date: 2026-11-03
- Author: dsp-engineer
- Scope: ADR-0011 §4.3 and §6 N3, first slice: `common/dsp/neural`
  (the WaveNet kernel), the `.nam` reader (`plugins/native/ampcab`), the
  equivalence test against the NAM reference implementation, and the
  class-E cost of the trainer's four WaveNet sizes (Unverified 9, the
  networks only).
- Where: `sonora-build:trixie` on the owner's laptop (11th Gen Intel Core
  i7-11800H, 2.30 GHz, 8 cores / 16 threads) under Docker Desktop on
  Windows. GCC 14, Release (-O3), SSE2 only (no -march). Other agents'
  containers were running: load average 10 to 15 during the timings
  below. These are container figures on a loaded host, not appliance
  figures.

## What the format means, and where it was read

The architecture and weight order are the NAM project's open `.nam`
format: the trainer (neural-amp-modeler, MIT; `nam/train/core.py`,
`get_wavenet_config`, for the four presets) and the reference
implementation (NeuralAmpModelerCore, MIT; `NAM/wavenet/model.cpp`,
`NAM/conv1d.cpp`, `NAM/activations.h`) at commit `0b3d3c9`. The kernel is
our C++17; nothing was copied except the `Fasttanh` coefficients, which
are part of the format's meaning (a file that names that activation was
trained against that function). Eigen (MPL-2.0) is used only by the
oracle build in `tests/oracle/generate.sh`, from Debian's
`libeigen3-dev`; it is not vendored or linked into any Sonora target
(ADR-0011 §4.3, "For the owner" item 1).

Accepted: format versions 0.5.x and 0.6.x; WaveNets of any depth of
layer arrays, any channel count up to 128, kernel up to 32, gated or not,
and the newer keys at their neutral values, within caps on the totals
(receptive field 65 536, history 4 Mi floats, 1 Mi weights, 4 096 layer
rows, 2^30 multiply-adds of warm-up; "standard" needs 4 092, 98 208,
13 802, 240 and 5.6e7). The totals cap came from review: a 52 KB file
within every per-dimension cap made prepare() run 19 s. Refused with a sentence: LSTM (follow-up),
bottleneck, grouped convolutions, FiLM, head1x1, the post-stack head, a
condition DSP, parameterised activations (LeakyHardtanh, PReLU).

## Equivalence (ADR-0011 §3.2: max |difference| <= 1e-5)

Our own test networks (seeded dyadic weights, so the JSON text names one
float exactly), on 8 192 samples of silence, a quiet sweep, a sweep 3.5 dB
over full scale, noise and a step. The reference ran at 64-frame blocks,
ours at 1, 37, 64, 128 and 1 000 frames (prepared for 128, 64 or 256).

| Network | Activations | Max \|ours - reference\| | Reference peak |
|---|---|---|---|
| tiny (3/2 channels) | Tanh | 5.6e-9 | 0.026 |
| gated (4/4) | Tanh gated, ReLU, kernel 2 | 8.9e-8 | 0.476 |
| mixed (five arrays) | Hardtanh, Softsign gated, Hardswish, LeakyReLU, Sigmoid | 4.5e-8 | 0.114 |
| feather (8/4) | Fasttanh | 1.7e-7 | 0.239 |
| standard (16/8, 13 802 weights) | Tanh | 1.3e-7 | 0.185 |

The same samples at every block size, to the bit (test_neural.cpp holds
that too, with reset and NaN/Inf input). The kernel's own tanh and
sigmoid (`src/neural/vec_math.h`, Cody-Waite exp) are within 2.5e-7 of
libm in double over [-20, 20].

## Class E: the networks alone, q128, 48 kHz, mono

`ampcab-budget 20` (ours) and `tests/oracle/generate.sh --bench 20` (the
reference, default build: Eigen, std::tanh), 7 500 blocks each, in the
same container run. CPU us is process CPU time per block, which leaves out
preemption by the other containers.

| Size | Weights | Ours p50 / p99.9 / max us | Ours CPU us | Reference p50 / p99.9 / max us | Reference CPU us |
|---|---|---|---|---|---|
| nano | 842 | 59 / 162 / 247 | 70 | 102 / 251 / 352 | 102 |
| feather | 3 026 | 155 / 353 / 418 | 188 | 149 / 413 / 895 | 162 |
| lite | 6 554 | 292 / 711 / 812 | 340 | 268 / 1 230 / 3 391 | 315 |
| standard | 13 802 | 560 / 2 146 / 7 149 | 687 | 394 / 1 029 / 2 056 | 435 |

Class E is p99.9 <= 250 us and max <= 500 us for one capture **plus** its
cabinet IR. On this loaded host:
- nano fits with room for the IR; feather's p50 fits and its tail does
  not, which on a loaded host says little;
- lite and standard do not fit with this kernel; standard costs about 1.6
  times the reference's Eigen product.

Not yet decided, and why: the tails above are the scheduler's, not the
kernel's. Unverified 9 stays open until the same two commands run on a
quiet machine (the appliance, or this laptop with nothing else running),
with the 200 ms cabinet IR beside them once the partitioned convolver
lands (#307).

## What would make standard fit (follow-ups)

- An x86-64 AVX2 product in `src/arch/x86_64/` with run-time selection
  and the generic one kept (ADR-0011 §2.3). Estimated from a 16 x 48 x
  128 product timed alone (21 us), the products are about two thirds of
  a standard block, at four lanes today.
- Eight output rows per pass instead of four (one input load per eight
  multiply-adds).
- tanh: the exp-based form costs about 145 us per 30 720 values here; a
  rational approximation held to the same 2.5e-7 would cost less.

## Slice 2: AVX2 kernels chosen at run time, rational tanh

What changed (common/dsp/src/neural/kernels.h, src/arch/x86_64/):
- The product, tanh and sigmoid are a kernel table. The generic table is
  the code above. `neural_avx2.cpp`, the only file compiled with `-mavx2`
  (`/arch:AVX2` on MSVC), holds eight output rows by eight frames in
  registers over the whole input loop. `neural_simd.cpp` picks it when
  CPUID says AVX2 and the OS saves the ymm state, so the library still
  runs on an x86-64 without AVX2.
- No FMA, every sum in the generic order: the AVX2 table gives the
  generic table's bits. `test_dsp_neural` holds this for 280 products
  (1 to 32 rows, 1 to 48 inputs, 1 to 130 frames), both activations at
  every tail length with NaN and Inf, and whole networks through
  `process()`. A capture sounds the same on every machine, and golden
  files do not depend on the runner's processor.
- tanh is now a rational x P(x^2)/Q(x^2), degrees 6 and 3, clamped at
  +-7.9. It is our own fit, with the method in `vec_math.h`. It is in
  both tables. Its error against std::tanh is 3.2e-7 (the bar moves from
  2.5e-7 to 3.5e-7: about 5 float ulps below 1, from the float rounding
  of the Horner sums; the fit alone is 1.4e-9). In AVX2 it takes 2.2 us
  per 2 048 values against 4.6 us for the exp-based form. The equivalence
  with the NAM reference is unchanged in kind: standard 1.38e-7, the
  worst file (feather) 1.71e-7, all under the 1e-5 bar.

Pieces timed alone (best of 2 000, i7-11800H, build image, GCC 14 -O3):

| Piece | Generic us | AVX2 us |
|---|---|---|
| conv product 16 x 48 x 128 | 20.7 | 7.2 |
| residual product 16 x 16 x 128 | 7.1 | 2.4 |
| tanh, 16 x 128 values (exp form) | 9.6 | 4.8 |
| tanh, 2 048 values (rational) | | 2.2 |

`ampcab-budget 20`, both tables in one run, 7 500 blocks each, q128,
48 kHz, mono. The host was heavily loaded by other containers: the
generic standard took 1 077 us p50 here against 560 us in the slice-1
run above. Only the ratio carries over.

| Size | Generic p50 / p99.9 / max us, CPU us | AVX2 p50 / p99.9 / max us, CPU us |
|---|---|---|
| nano | 132 / 1 972 / 4 333, 155 | 38 / 532 / 2 214, 42 |
| feather | 329 / 2 420 / 7 575, 362 | 89 / 914 / 3 369, 101 |
| lite | 604 / 2 738 / 6 708, 640 | 175 / 1 571 / 2 798, 196 |
| standard | 1 077 / 5 236 / 5 919, 1 136 | 289 / 1 433 / 4 489, 291 |

AVX2 runs 3.5 to 3.9 times faster than generic. Applied to the slice-1
generic figure (560 us p50), standard would come to about 150 us p50
and lite to about 85 us. That is an estimate, not a measurement.
Unverified 9 stays open until `ampcab-budget` runs on a quiet machine
with the cabinet convolver (#307) beside it.

What is left if standard misses class E on the quiet run:
- FMA. It halves the arithmetic of the products, which is the floor here:
  2.6 M multiply-adds per standard block is about 110 us at 3 GHz
  without FMA. But FMA changes bits against the generic table. A
  capture would then sound different, in the last bits, on an FMA and a
  non-FMA machine, and the golden-file rule (ADR-0011 §2.3, §3.4) would
  need an amendment. This needs a decision before any code.
- The element-wise passes (bias and mixin, head sum, residual add) still
  run at the generic width.
