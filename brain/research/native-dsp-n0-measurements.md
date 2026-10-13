# Native DSP, stage N0: what was measured

ADR-0011 (proposed) N0: `common/dsp`, `plugins/native/sdk`, the harness
(`plugins/native/measure`, `sonora-dsp-measure`) and six processors. Every
figure below comes from a run of the code on `feature/dsp-core`; how to
reproduce it is at the end. Measured 2026-10-13.

**Machine:** Intel Core i7-11800H (8 cores, 16 threads), Debian trixie
container under Docker Desktop on Windows 11, GCC 14, `-O3` (Release),
`-ffp-contract=off`, no `-ffast-math`. This is the owner's laptop, but not
bare metal and not the RT kernel. ADR-0011 Unverified 4 stays open for the
reference machine.

## Filter designs against their analog prototypes

`common/dsp/tests/test_filters.cpp`, 266 designs over 3 rates. Error is the
worst magnitude difference from 20 Hz to 20 kHz where the target is at or
above -20 dB.

- **Bilinear (RBJ cookbook) cramps:** a 15 kHz, Q 2, +12 dB bell at 48 kHz
  is 4.83 dB off. Vicanek's matched bell gets 0.37 dB at 1x and 0.009 dB
  at 2x.
- **No single second-order section reaches ADR-0011's ±0.1 dB at 44.1 or
  48 kHz for wide upper bells.** A numerically optimal biquad (least squares
  on all five coefficients) for a 10 kHz, Q 1, +18 dB bell at 44.1 kHz is
  still 0.42 dB off. The analog bell rises past Nyquist, and a digital
  magnitude must fold back there.
  - Matched at 1x (the EQ's zero-latency mode), worst case: bells 1.4 dB,
    shelves 0.32 dB.
  - The bar holds below about 2 kHz for most shapes; a Q 0.4 bell at 1 kHz
    is 0.18 dB off.
- **Run at twice the session rate (the EQ's default "analog matched"
  mode), every EQ shape is within 0.1 dB.**
  - Worst cases: bells 0.055 dB, shelves 0.037 dB, low-pass 0.035 dB,
    high-pass 0.008 dB (corner at or below fs/8).
  - Phase is within 5 degrees of the prototype for f0 up to 2 kHz, and up
    to 8.5 degrees above that. A minimum-phase section's phase follows its
    own magnitude, which ends at the digital Nyquist frequency.
- **Through the EQ processor** at 2x, on nine composite curves at 44.1, 48
  and 96 kHz, the worst magnitude error is 0.029 dB and the worst phase
  error 0.84 degrees (`docs/images/dsp/eq-response-2x.png`).
- **Design choices measured, not assumed:**
  - Vicanek's published high-pass keeps a double zero at DC. A three-point
    fit puts one zero off DC and passes LF 30 dB above the analog stopband.
  - Shelves are our own: matched poles, with the numerator matched at DC,
    f0 and 2 f0. Matching DC, f0 and Nyquist instead is up to ten times
    worse at 88.2 kHz.
  - Cuts are designed as the inverse of the matching boost.
- **The SVF realisation is exact:** its impulse response matches the
  direct form to 1.4e-13.

## Oversampling

Linear-phase half-band FIR cascade, 100 dB stopband, measured with an
impulse at 44.1, 48 and 96 kHz:

| Factor | Reported latency | Measured latency |
|---|---|---|
| 2x | 69 | 69 |
| 4x | 76 | 76 |
| 8x | 78 | 78 |

Passband is within 0.01 dB to 20 kHz. A hard clipper on 9 kHz at 48 kHz
puts this much non-harmonic energy in the band:

| Factor | Non-harmonic energy |
|---|---|
| 1x | -14.3 dBc |
| 2x | -31.2 dBc |
| 4x | -43.0 dBc |
| 8x | -55.7 dBc |

The tapered harmonics of a hard clip reach past even 8x. ADAA or a softer
curve is N2's subject.

## Loudness (BS.1770-5, EBU Tech 3341/3342 v4, 2023)

- Every generated minimum-requirement case passes at 44.1, 48 and
  96 kHz: Tech 3341 cases 1-6, 9-14 (I, M, S, live and file-based) and
  15-23 (true peak), and Tech 3342 cases 1-4.
- The cases that need EBU's authentic programme files are not run: 3341
  cases 7-8 and 3342 cases 5-6. Whether those files may be redistributed
  is ADR-0011 Unverified 1.
- K-weighting reproduces the coefficients printed for 48 kHz to 1e-6.

## Limiter: what the ceiling test taught

The first stage's stress set is 7 band-limited signals x 3 rates x 2
ceilings x 2 releases, judged by a 16x reference meter of ±256 taps.

| Step | Worst over |
|---|---|
| First version (plain 4x, 32 taps per phase) | +1.2 dB |
| Fades on the stress signals, as Tech 3341 does for its own (abrupt onsets are not band-limited) | 0.3 dB on noise |
| Sinusoidal refinement between the 4x points (a 4x grid misses peaks by up to 1 - cos(pi f/4fs), 0.5 dB at 20 kHz) | +0.04 dB |
| 96 taps per phase | +0.023 dB |
| 0.05 dB internal safety margin | zero overs (worst -0.027 dB under) |

Tried and rejected for that stage: a wider guard window (no effect), a
second identical stage (it cannot see what its own detector misses), and
triangular gain smoothing (no effect on the over).

## Limiter: the ceiling guarantee (PR #34 review)

The coordinator ruled that a mastering limiter never exceeds its ceiling.
Full-band white noise, which the first stage let through at +0.68 dB on the
reference, is now asserted, on two meters: the BS.1770 4x meter (common/dsp's,
32 taps per phase, as `loudness.h` runs it) and the 16x reference.

What the first stage alone does on full-band white noise at 48 kHz, ceiling
-1 dBTP, release 5 ms:

| Meter | Highest true peak, relative to the ceiling |
|---|---|
| Sample peak | -2.64 dB |
| 16x reference | +0.689 dB |
| BS.1770 4x meter | +0.292 dB |

Two separate failures:
- The first stage's interpolator rolls off from about 0.47 fs, so the
  reference sees near-Nyquist peaks that the first stage does not.
- The BS.1770 meter's own 4x interpolator passes part of the images of
  near-Nyquist content. On the unlimited noise it reads up to 2.9 dB above
  the reference.

A ceiling has to hold on the meter a broadcaster's QC will use, as well as
on the true reconstruction.

**The guard** (`dsp::TruePeakGuard`) runs after the first stage. It
detects on the first stage's output, so the sidebands that stage's gain
makes are measured rather than predicted. It takes the larger of two
readings:
- 8x through the half-band cascade, each local maximum refined by the
  sinusoidal fit;
- the BS.1770 meter's own 4x reading of the same interval.

It then applies a second look-ahead gain (1 ms) at the same 0.05 dB target.
Its design, each step measured on full-band noise (3 rates, 2 levels, 3
seeds, guard release 5 ms):

| Guard detector | Guard latency at 48 kHz | Worst, 16x reference |
|---|---|---|
| 8x, passband 0.49 fs, 80 dB | 178 | +0.143 dB |
| 8x, passband 0.495 fs, 80 dB | 304 | -0.005 dB |
| 8x, passband 0.4975 fs, 80 dB | 555 | +0.056 dB |
| 8x, first stage = the reference's own filter (Kaiser 120 dB, 4 x 256 - 1 taps at 2x) | 310 | -0.028 dB |
| the same, aiming deeper in proportion to its own cut (need in dB x 1.15) | 310 | -0.050 dB |

- A wider passband is not better on its own. What matters is reading the
  last per cent below Nyquist through the same transition band as the
  meter that judges it, and matching the reference's filter does that.
- The rest is the guard's own sidebands. It detects before applying its
  gain, and that gain puts sidebands on near-Nyquist content which fold
  back. Their effect grows with the depth of the cut, so the guard aims
  deeper in proportion (x 1.15 in dB). That keeps the full 0.05 dB margin.
- A 2 ms guard look-ahead measured no better than 1 ms, so it is 1 ms.
- A final clamp holds the sample peak at the ceiling. The tests assert it
  never acts.

**Result.** `common/dsp/tests/test_limiter.cpp`, run in full: 11 signals
(the 7 band-limited ones, and full-band white noise at +6, +12 and +24 dB
with two seeds at +12 dB) x 3 rates x 2 ceilings x 2 releases = 132 runs.
- Zero overs.
- The highest true peak relative to the ceiling is -0.0490 dB on the 16x
  reference and -0.0500 dB on the BS.1770 meter.

The native limiter's suite asserts the same through the processor: 7
signals, full-band noise included, x 3 rates x 2 ceilings = 42 runs, zero
overs, highest -0.0495 dB on the reference.

**What it costs.**
- Latency: the limiter at its defaults reports 430 samples at 48 kHz (was
  120), 420 at 44.1 kHz and 550 at 96 kHz. Measured latency equals reported
  latency in every configuration.
- CPU: 80.1 µs at p50 and 86.5 µs at p99.9 code path (was 32.3 and 35.5).
  That is inside class C (150 / 300). The single-run max inside Docker
  Desktop was 355 µs, over class C's 300; that figure is the container's
  scheduling, as for the other processors below.
- Distortion, through the processor at its defaults, on tones 6 dB over the
  ceiling (THD+N, both stages / first stage alone):

  | Tone | Both stages | First stage alone |
  |---|---|---|
  | 1 kHz | -151.1 dB | -151.1 dB |
  | 5 kHz | -147.1 dB | -149.6 dB |
  | 10 kHz | -148.8 dB | -149.4 dB |
  | 15 kHz | -157.5 dB | -164.6 dB |
  | 19 kHz | -157.2 dB | -157.2 dB |

  Non-harmonic energy gives the same figures (no harmonics survive):
  -147 dBc at worst, against the -90 dBc bar.

On band-limited programme the guard trims what the first stage's 4x
detector under-reads. Release 50 ms, ceiling -1 dBTP, 48 kHz:

| Signal | First stage alone, 16x | Guard's deepest cut | Change, relative to the output |
|---|---|---|---|
| noise to 20 kHz, +12 dB | -0.041 dB | 0.011 dB | -67.7 dB |
| square 1 kHz to 20 kHz, +6 dB | -0.032 dB | 0.022 dB | -52.0 dB |
| log sweep, +12 dB | -0.050 dB | 0.002 dB | -86.0 dB |
| drum-like bursts, +18 dB | -0.048 dB | 0.003 dB | -80.0 dB |
| full-band white noise, +12 dB | +0.680 dB | 0.842 dB | -28.7 dB |

On full-band noise the change is the guard's extra 0.7 to 0.8 dB of gain
reduction, ramped. It is a level change, not added harmonics.

## CPU per instance

`tools/dsp-cpu.sh build-container 60`: q128, 48 kHz, stereo, 60 s of
noise plus a sweep. Re-measured 2026-10-13 after the review fixes.
- The "code path" figure is the per-block minimum over 5 identical
  renders. A deterministic render does the same work on block k every
  time, and container scheduling noise rarely hits the same block five
  times.
- The single-run figures include that noise.

| Processor (class) | p50 µs | p99.9 µs code path | max µs code path | p99.9 µs single run | max µs single run | Class bar p99.9 / max |
|---|---|---|---|---|---|---|
| EQ, 2x (A) | 16.7 | 18.1 | 18.3 | 78.1 | 153.6 | 20 / 50 |
| EQ, 1x (A, eco) | 5.6 | 6.1 | 6.2 | 27.4 | 90.8 | 20 / 50 |
| Compressor (A) | 9.7 | 10.6 | 10.9 | 36.3 | 158.3 | 20 / 50 |
| Gate (A) | 6.6 | 7.2 | 7.4 | 24.8 | 59.1 | 20 / 50 |
| Limiter with the guard (C) | 80.1 | 86.5 | 87.9 | 237.6 | 354.9 | 150 / 300 |
| Delay (A) | 2.3 | 2.6 | 2.7 | 19.7 | 137.2 | 20 / 50 |
| Chorus, 3 voices (A) | 12.2 | 13.0 | 13.2 | 42.8 | 187.4 | 20 / 50 |

- The code-path figures meet every class. That is the figure the class bar
  is asserted on (ADR-0011 Amendment 1, item 4, the coordinator's
  decision).
- The single-run figures miss class A's max everywhere, and class C's for
  the limiter. That is Docker Desktop's scheduling (a 2.3 µs delay block
  measured 137 µs). Only a bare-metal RT run can say what the appliance
  sees.
- Two fixes came out of the first measurements:
  - folding the half-band FIR and summing it into four accumulators took
    the 2x EQ from 50 µs to 17 µs;
  - rotating one sin/cos per sample into each voice's phase took the
    chorus from 22 µs to 12 µs.

## Other findings

- **Compressor aliasing.** At 20:1 with a 0.05 ms attack and 5 ms release,
  a 5 kHz tone carries -46 dBc of non-harmonic energy. ADR-0011 lists no
  aliasing bar for the compressor. An oversampled option belongs with the
  analog-character work (N2).
- **Measurement artefacts fixed in the harness**, each of which made a
  correct processor look broken:
  - A response taken from the latency onwards cut a linear-phase stage's
    pre-ringing.
  - A Blackman-Harris window floors THD+N at about -90 dB, so the harness
    uses Kaiser beta 20.
  - A zipper tone that starts abruptly is echoed by a delay into the
    measured window, so the tone is faded in.
  - A limiter's output cut off at the input's length ends in the middle
    of the delayed signal, and the reference reconstructs that edge as a
    step with its own overshoot (it read a log sweep at -0.008 dB). The
    ceiling tests append the latency's worth of silence first.
- **The reference meter skipped peaks it should have read.** It evaluated
  only between samples where either one was within 6 dB of the level.
  Near Nyquist a peak can sit between two samples near zero; at 20 kHz
  and 44.1 kHz the samples either side of a peak are 16.7 dB under it. It
  now pre-scans at 4x through the half-band cascade (passband 0.495 fs)
  and evaluates the 16x reconstruction around every local maximum that
  could reach the level (`common/dsp/include/sonora/dsp/reference_peak.h`).
  It reads the same as a full evaluation on every case tried. It is also
  faster: the full limiter test went from 63 s to 24 s, with four more
  signals in it.
- **A finite 1e30 sample is as harmful as a NaN.** The old check asked
  only for finite output. Asked for "back to normal within a block", five
  of the six processors failed: the limiter sat 600 dB down for its
  release, and the EQ, delay and chorus carried 1e29-1e30 for seconds.
  `ProcessorBase` now treats input beyond ±1e6 (+120 dBFS) as it treats
  NaN, and the check asserts that from the next block on the output equals
  the render with silence in those samples' place.
- **The delay line was 4 s; the longest note at the slowest tempo is
  12 s** (1/1 at 20 BPM), and it was silently cut to 4 s. The line is now
  sized for 12 s (8 MiB a channel at 96 kHz, as the line rounds up to a
  power of two). A host tempo under 20 BPM is bounded at 20 BPM and counted
  in `ProcessorBase::bounded_blocks()`, which the harness asserts.
- **The denormal check measured noise.** It compared the median block
  cost in seconds 2-10 with seconds 15-20 of one render; on a 2.4 µs delay
  block that read 1.49 once in five runs (bar 1.2) with nothing wrong. It
  now times a quiet instance and an active one side by side, one block of
  each in turn. That also makes it safe under `ctest -j`.
- **Zipper lists were incomplete.** The harness now fails a `quality.json`
  whose zipper list leaves out an automatable parameter. It found 14 left
  out (EQ bells 2 and 3 and bell 4's switch; the limiter's detection and
  link; the delay's note values and tempo). The delay sweeps every
  parameter twice: with sync off, where the times are heard, and with sync
  on, where the note values and the tempo are.

## Reproduce

    tools/build-engine.sh                       # all ctests, bars asserted
    SONORA_DSP_QUICK=1 ctest --test-dir build-container -L slow
                                                # the slow set as PR CI runs it
    build-container/sonora-dsp-measure --processor eq \
        --quality plugins/native/eq/quality.json \
        --golden plugins/native/eq/tests/golden \
        --report build-container/dsp-reports --cpu
    tools/dsp-cpu.sh build-container 60         # §3.5 CPU, every processor
    tools/dsp-plots.py build-container/dsp-reports docs/images/dsp
