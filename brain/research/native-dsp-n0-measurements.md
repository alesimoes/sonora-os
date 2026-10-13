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

The stress set is 7 band-limited signals x 3 rates x 2 ceilings x 2
releases, judged by a 16x reference meter of ±256 taps.

| Step | Worst over |
|---|---|
| First version (plain 4x, 32 taps per phase) | +1.2 dB |
| Fades on the stress signals, as Tech 3341 does for its own (abrupt onsets are not band-limited) | 0.3 dB on noise |
| Sinusoidal refinement between the 4x points (a 4x grid misses peaks by up to 1 - cos(pi f/4fs), 0.5 dB at 20 kHz) | +0.04 dB |
| 96 taps per phase | +0.023 dB |
| 0.05 dB internal safety margin | zero overs (worst -0.027 dB under) |

Tried and rejected: a wider guard window (no effect), a second
limiter stage (it cannot see what its own detector misses), and
triangular gain smoothing (no effect on the over).

**Full-band white noise** (content at full scale up to Nyquist) still reads
+0.68 dB on the reference. No finite detector sees it all. It is reported,
not asserted.

## CPU per instance

q128, 48 kHz, stereo, 60 s of noise plus a sweep. The "code path"
figure is the per-block minimum over 5 identical renders: a deterministic
render does the same work on block k every time, and container scheduling
noise rarely hits the same block five times. The single-run figures
include that noise.

| Processor (class) | p50 µs | p99.9 µs code path | max µs code path | p99.9 µs single run | max µs single run | Class bar p99.9 / max |
|---|---|---|---|---|---|---|
| EQ, 2x (A) | 17.4 | 18.1 | 18.4 | 48.0 | 166.9 | 20 / 50 |
| EQ, 1x (A, eco) | 5.7 | 6.0 | 6.5 | 33.3 | 71.5 | 20 / 50 |
| Compressor (A) | 9.9 | 10.6 | 11.0 | 30.8 | 91.8 | 20 / 50 |
| Gate (A) | 6.9 | 7.3 | 7.7 | 32.4 | 68.7 | 20 / 50 |
| Limiter (C) | 32.3 | 35.5 | 36.5 | 84.0 | 132.4 | 150 / 300 |
| Delay (A) | 2.4 | 2.6 | 2.8 | 20.0 | 56.6 | 20 / 50 |
| Chorus, 3 voices (A) | 12.3 | 13.0 | 13.1 | 47.0 | 96.2 | 20 / 50 |

- The code-path figures meet every class.
- The single-run figures do not meet class A's max anywhere. That is
  Docker Desktop's scheduling (a 2.4 µs delay block measured 57 µs), and
  only a bare-metal RT run can say what the appliance sees.
- Two fixes came out of these measurements. The half-band FIR, folded and
  summed into four accumulators, took the 2x EQ from 50 µs to 17 µs.
  Rotating one sin/cos per sample into each voice's phase took the chorus
  from 22 µs to 12 µs.

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

## Reproduce

    tools/build-engine.sh                       # all ctests, bars asserted
    build-container/sonora-dsp-measure --processor eq \
        --quality plugins/native/eq/quality.json \
        --golden plugins/native/eq/tests/golden \
        --report build-container/dsp-reports --cpu
    tools/dsp-plots.py build-container/dsp-reports docs/images/dsp
