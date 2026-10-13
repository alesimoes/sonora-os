# Native reverb, stage N2 slice 1: what was measured

- Date: 2026-10-13
- Author: dsp-engineer
- Scope: ADR-0011 §6 N2, its first slice: the FDN reverb core in
  `common/dsp` (`fdn.h`) and `plugins/native/reverb`, with its LV2 and
  VST3 bundles through N1's format layer. The rest of N2 is sliced in
  `planning/BACKLOG.md`.
- Where: `sonora-build:trixie` and `sonora-plugins:trixie` containers on
  the owner's laptop (11th Gen Intel Core i7-11800H, 2.30 GHz) under Docker
  Desktop on Windows. GCC 14, Release. The CPU figures are container
  figures, not appliance figures (ADR-0011 Amendment 1 item 4).

## Design, and where each part comes from

| Part | Source | Our choice |
|---|---|---|
| Network | J.-M. Jot and A. Chaigne, AES 90th Convention, 1991, preprint 3030 | 16 delay lines, lengths geometric from 25 to 83 ms at size 100 % (15 % of that at size 0), each rounded to a distinct prime |
| Feedback matrix | S. J. Schlecht and E. A. P. Habets, IEEE TSP 65(6), 2017 (lossless FDNs) | Hadamard of order 16 scaled by 1/4: orthogonal, and 1/4 is exact in binary floating point |
| Frequency-dependent decay | Jot and Chaigne: per-line attenuation proportional to its length | Gain, low shelf and high shelf per line, shelves by `design_matched()` (Vicanek 2016) at Q = 1/sqrt 2 |
| Diffusion | M. R. Schroeder, JAES 10(3), 1962 | Four series allpasses per channel, 1.3 to 5.9 ms, g from 0.1 to 0.7 |
| Measurement | ISO 3382-1:2009 | T30 per octave band, time-reversed sixth-order Butterworth band filters, Schroeder backward integration, energy averaged over the four input/output responses |

No third-party code. Nothing was taken from any product, reverb plugin or
impulse response (ADR-0004, ADR-0011 §5).

Two choices are ours and are the ones a reviewer should look at:

1. **Outputs read the lines' inputs**, after the matrix and the injection,
   through sign vectors whose Walsh-Hadamard spectra are flat (bent
   functions, found by search and checked in `tests/test_fdn.cpp`). Every
   line then reaches each output with equal weight through the matrix,
   and the left and right outputs are orthogonal. The injection vectors
   have a dot product of 2 with both outputs, so the first wet sample is
   1/8 (-18 dB) of the input, exactly at the pre-delay, whatever the size.
   A larger direct term was tried first (1, the injection equal to the
   output vectors): the wet then opens with a full-level copy of the dry
   signal, a slap echo that dominates the response's energy at short
   decays (estimated from the network's energy balance, not measured).
   Zero would put the onset at the first line's return,
   4 to 30 ms after the pre-delay depending on size; the pre-delay a tempo
   grid sets (ADR-0011 §4.4) would then not be the one heard.
2. **Size and pre-delay changes crossfade taps** over 30 ms, as the
   delay's time changes do. No fractional read in the loop: Lagrange-3 at
   a half-sample fraction loses 0.23 dB at 8 kHz and 48 kHz per pass,
   which at a 50 ms line and a 2 s decay would shorten T60 there by about
   13 % (computed from the interpolator's response, not measured).

## Results (ctest `dsp_fdn` and `native_reverb`; `tools/build-plugins.sh`)

| Check | Result | Bar |
|---|---|---|
| Line filters, every rate, size and setting on a grid to T60 = 40 s | largest \|H\| -0.0055 dB | below 0 dB (stable) |
| Network alone, broadband T30, 0.5 and 2 s at 44.1/48/96 kHz | -0.37 % to +0.05 % | 2 % |
| Freeze, line energy over 10 s | relative drift 2e-8 | 1e-4 |
| Reverb T30 per octave band, 171 bands held (flat decays 0.4, 1.5, 6 s; sizes 0, 50, 100 %; each shelf alone; shaped; defaults; 44.1/48/96 kHz) | worst 2.3 % (44.1 kHz), 1.6 % (48 kHz), 2.0 % (96 kHz) | 5 % |
| Onset: first wet sample at the pre-delay (0, 10, 37.5, 250 ms; sizes 0 and 100 %; stereo and mono) | exact, every case | exact |
| Latency reported; bypass is the input delayed by it | 0; exact | exact |
| Freeze: 1 s RMS of the held tail over 9 s | 0.042 dB spread | 0.5 dB |
| Freeze: input after the 50 ms ramp | changes nothing (0) | 1e-6 |
| Width 0 %: left against right | identical | identical |
| Width 100 %: \|interchannel correlation\| of the tail, mono impulse | 0.034 | 0.2 |
| THD+N, 1 kHz at -1 dBFS, wet steady state | -148.8 dB | -110 dB |
| Decay 20 s, low x2 (40 s), 10 s of noise at -6 dBFS: peak | 2.19 | bounded (20) |
| Zipper, all 13 automatable parameters, full-range sweeps | -94 dB (size) to -150 dBFS | -60 dBFS |
| Non-finite and 1e30 input | finite; same as silence from the next block | exact |
| Denormals, silence after signal against active | 1.00 | 1.2 |
| Golden renders, 44.1/48/96 kHz | 0 difference | 1e-6 |
| lv2lint, stereo and mono bundles | pass | pass |
| VST3 validator 3.8.1, stereo and mono | pass | pass |
| Through `sonora-plugin-host`, LV2 and VST3: golden x3, bypass, automation (decay moved mid-render), mono | 14 of 14 bit-exact against the reference | 1e-6 |
| ASan and UBSan (`dsp_*`, `native_*`), TSan (`native_format_*`, the reverb bundles from two threads) | clean | clean |

The plot is `docs/images/dsp/reverb-t60-bands.png` (`tools/dsp-plots.py`).

### CPU, ADR-0011 §3.5 class C (150 us p99.9, 300 us max)

Per 128-frame block at 48 kHz, stereo, 60 s renders, the per-block minimum
of five runs (Amendment 1 item 4):

| Case | p50 | p99.9 | max |
|---|---|---|---|
| Parameters still | 22.0 to 22.6 us | 23.3 to 42.2 us | 25.4 to 49.1 us |
| Decay and both crossovers automated on every block (every line redesigned each 32 samples) | 54.7 us | 61.7 us | 71.4 us |

Two runs; the spread in p99.9 is Docker Desktop's scheduling. The
single-run max reached 188 us and 2990 us (host preemption, not the code
path). The still case would fit class B (60 us).

## What the measurement cannot hold, and why

**Low bands at short decays.** An octave band's decay curve has about
B x T60 degrees of freedom (B the bandwidth). Below a product of about 100
a single curve fluctuates by more than the 5 % bar even for an ideal
exponential decay: the harness's own check, four realisations of decaying
white noise through the same routine, reads -2 % to -5 % at 63 to 250 Hz
for T60 = 0.4 s, and -3.4 % at 63 Hz for 1.5 s. Those bands are reported,
not held; on the reverb they read within 3 % (flat cases) at 48 kHz. ISO
3382-2:2008 treats this measurement uncertainty (§7.2 and Annex A); its
formula was not checked against these figures.

**Crossover transitions.** An octave band that straddles a shelf's
transition mixes two decay rates, and its T30 follows the slower, which
dominates late in the curve: 44 % above the design value at the 1 kHz band
for a high decay of x0.3 at 1 kHz, 68 % at 4 kHz in the shaped case. That
is the measurement averaging over a band, not the filter: the bands an
octave or more away from a crossover are held to the bar and read within
2.3 %. The design curve in the plot is evaluated at the band centre.

**Matched shelves against the analog design.** In the worst case on the
`dsp_fdn` grid (44.1 kHz, T60 20 s, high x0.1 at 16 kHz, at 7.4 kHz) the
matched shelf's attenuation is 25 % off the analog prototype's, relative to
a very small attenuation there. It is reported by `dsp_fdn`, not held.

## Not done in this slice

Listed in `planning/BACKLOG.md` under ADR-0011:
- slow modulation of the line lengths against metallic ringing at long
  decays, with a figure for it first;
- a separate early-reflection stage;
- calibration and listening against measured rooms (Arni, Motus; CC BY
  4.0, `open-data-and-dsp-sources.md` §C.1);
- the rest of N2: SPICE harness, triode, transformer, tape, noise and hum,
  opto, FET, mix bus, tuner.
