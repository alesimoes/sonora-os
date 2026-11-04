# Mix bus, stage N2 slice 7, part 1: the bus compressor and width

- Date: 2026-11-03
- Author: dsp-engineer
- Scope: ADR-0011 §6 N2, slice 7 of `planning/BACKLOG.md`, first part: the
  processor `plugins/native/mixbus` ("Sonora Mix Bus", `org.sonora.mixbus`)
  with its bus compressor and its stereo width stage, its suite and its LV2
  and VST3 bundles, stereo and mono. The console colour (a line output
  transformer, oversampled, on slice 3's Jiles-Atherton core) is part 2,
  with its own bars and the class-B CPU figure it will need.
- Where: the `sonora-build:trixie` image (Debian trixie, GCC 14) on the
  owner's laptop (11th Gen Intel Core i7-11800H), under Docker Desktop on
  Windows, Release build. CPU figures are container figures, not appliance
  figures (ADR-0011 Amendment 1 item 4).

## Design, and why

`native-plugin-suite.md` §1.12 lists the mix-bus processor as a composition:
a gentle VCA-style compressor (low ratio, slow attack, auto release, a
side-chain high-pass, make-up, mix), transformer or tube colour, a tilt EQ
and stereo width. The classic console bus compressors are VCA designs with
no paper to cite, so the claim here is functional, not a clone of any unit
or of any plugin (ADR-0004, ADR-0011 §3.3).

**Topology: feed-forward.** The research note allows feedback "for glue".
Our algebra rules it out against the ADR's bars. With the detector on the
output and a gain computer of ratio R and threshold T, the settled
reduction c satisfies c = (1 - 1/R)(y - T) with y = x - c, so
y - T = (x - T) / (2 - 1/R): the effective ratio is 2 - 1/R, never above
2:1. Correcting the slope to R - 1 restores the static ratio, but then the
loop's pole is a / (1 + (1 - a)(R - 1)) for a detector coefficient a: the
measured time constant shrinks with the ratio and the depth, so the ±10 %
bar on displayed attack and release (§3.2) cannot hold. Giannoulis,
Massberg and Reiss (JAES 60(6), 2012) make the same point qualitatively. A
one-sample feedback loop is also unstable at 10:1 with a 0.1 ms attack
(|pole| > 1 for R > 1 + (1 + a)/(1 - a), 9.9 at 44.1 kHz). So the
compressor is the feed-forward, log-domain design of the N0 compressor:
quadratic soft knee, branching peak detector on the reduction.

**Stepped ratios.** 1.5, 2, 4 and 10:1 (§1.12). A choice parameter whose
smoothed position maps log-linearly between adjacent ratios, so automating
it ramps the curve over 20 ms instead of stepping the gain; the common
zipper sweep holds that.

**Fully linked.** One reduction for both channels, from the louder key: a
bus compressor that moved the image would be a fault.

**Auto release, a design of ours.** Two more branching detectors run on
the same reduction: a fast path (the set attack, release 100 ms) and a slow
path (attack 600 ms, release 1.2 s); the reduction is the larger. A
transient barely charges the slow path and recovers in 100 ms; a sustained
stretch charges it and the recovery takes 1.2 s, without pumping. All three
paths run always, so the switch is a 20 ms crossfade of settled states. The
two times are held by the suite: after a 10 ms overload the 63 % recovery
is the fast path's, after 4 s the slow path's (it is then charged to
1 - e^(-4/0.6) = 99.87 %, so the expected time is 1.2 x 0.9987 s).

**Width.** M = (L + R) / 2, S = (L - R) / 2, out = M ± w S, w from 0 to
200 %. It runs on the compressor's rounded float output, where the sum and
difference are exact in double, so 100 % is the identity to the bit and the
parallel mix's "0 % is the dry input" stays exact through it.

**Latency.** None: no look-ahead, no oversampling, no nonlinearity in the
audio path beyond the gain modulation, whose aliasing is reported.

## Bars (quality.json) and measured figures

`sonora-dsp-measure --processor mixbus`, `SONORA_DSP_QUICK=1`, 4.3 s for
the whole suite (ctest `native_mixbus`).

| Measure | Bar | Measured |
|---|---|---|
| Static curve, 4 ratios x 21 DC levels, -40 to 0 dBFS | ±0.1 dB | 5.6e-7 dB |
| Manual attack 0.3, 10, 30 ms; release 50, 300, 1200 ms; 44.1, 48, 96 kHz | ±10 % | worst 5.8 % (0.3 ms at 44.1 kHz: one sample of 0.3 ms is 7.6 %); every other case under 0.4 % |
| Auto release after a 10 ms overload (fast path, 100 ms) | ±10 % | 100.0 ms |
| Auto release after 4 s of reduction (slow path, 1.2 s) | ±10 % | 1193.4 ms (0.55 %) |
| Side-chain high-pass at 150 Hz: 40 Hz tone's reduction falls by | ≥ 15 dB | 17.1 dB (17.8 to 0.7 dB) |
| Side-chain high-pass at 150 Hz: 1 kHz tone's reduction moves by | ≤ 0.1 dB | 0.009 dB |
| THD+N at -1 dBFS, 1 kHz, below threshold | ≤ -110 dB | -158.4 dB |
| Mix 0 %; width 100 % below threshold | exact | 0 |
| Width 0, 50, 150, 200 % against M ± w S | ≤ 1e-6 | 3.0e-8 (float rounding) |
| Width 0 %: L = R | exact | 0 |
| Zipper, every automatable parameter (width also on a left-only tone) | ≤ -60 dBFS | worst -96.8 dBFS (attack) |
| Latency, 2 configurations x 3 rates; bypass | exact | 0, 0 |
| Non-finite input, reset, denormals | as §3.2 | pass; silent tail / active CPU 0.84-0.88 |
| Golden, 44.1, 48, 96 kHz | 1e-6 | 0 |
| Aliasing of the gain modulation, 5 kHz at -6 dBFS, 10:1, 0.1/50 ms | reported | -59.1 dBc |

The aliasing figure is the gain modulation's sidebands at the fastest
attack and deepest ratio on a 5 kHz tone, as the N0 compressor reports it:
not a nonlinearity of the audio path, and far from the bus settings
(slower attack, lower ratio; not measured there yet). It is reported,
not barred; part 2's colour stage brings the aliasing bar (§3.2, -90 dBc
in the default quality) with its oversampler.

**CPU (§3.5, class B: p99.9 60 µs, max 150 µs).** `--cpu`, q128, 48 kHz,
stereo, 60 s of the harness programme, per-block minimum of 5 runs:
p50 24.4 µs, p99.9 29.7 µs, max 30.5 µs. Within class B with half its
budget left for part 2's colour stage; over class A (20 µs), which is not
its class. The cost is the per-sample dB conversions and the three
detectors; the slope and knee could run at a control rate if part 2 needs
the room.

## Part 2: the console colour (2026-11-03)

- Design: the line output transformer of `common/dsp/transformer.h` (a
  driver into a Jiles-Atherton core, with the supply's sag), after the bus
  compressor and before the width stage, run at the oversampled rate and
  mixed in by `colour` at that rate, so the dry path is aligned by
  construction. New ids appended: `quality` (eco 2x, standard 4x, high 8x;
  default standard; reconfigures), `colour` (0-100 %, default 0) and `sag`
  (default 100 %). Part 1's ids and indices do not move.
- Latency: the oversampler's round trip, always (so PDC does not move with
  the amount): 39 / 45 / 47 samples at 48 kHz for eco / standard / high
  (69 / 76 / 78 at 44.1, 11 / 16 / 18 at 96); measured equals reported at
  every quality and rate. The detector sees the undelayed input.
- Colour 0 %: output equals the compressor's, delayed, to the bit, at
  every quality (the transformer is not run and its state dropped).
- Harmonics (standard, colour 100 %, sag 0, -12 dBFS): 1 kHz total 2-9 at
  -58.2 dB, 40 Hz at -30.1 dB (27.4 dB more at LF). Every harmonic above
  -90 dBc agrees with the model driven by an ideal oversampled sine to
  0.17 dB (bar 0.5).
- Sag: a held 100 Hz tone at -1 dBFS ends 0.107 dB lower with sag 100 %
  than at 0 %.
- Aliasing (colour 100 %, 48 kHz, worst of tones 1-19 kHz): nominal -18
  dBFS eco -92.95, standard -96.96, high -96.53 dBc; hot (-6 dBFS) eco
  -61.8, standard -85.7, high -85.3 dBc, reported.
- CPU, q128, 48 kHz, stereo, per-block minimum of 5 runs, on the owner's
  laptop in the container with other containers running (reference kernel
  83.8 us here against 32.6 us on the reference machine, so x2.57):
  eco colour 0 % p99.9 52.8 us, colour 100 % 88.1 us (92.7 us at +12 dB),
  i.e. 34.3 and 36.1 us on the reference machine, inside class B (60 us).
  Uncalibrated on this loaded machine eco colour 100 % reads 82.7-91.1 us,
  over the bar: the machine, not the stage. Standard colour 100 % reads
  137.6 us (uncalibrated) and high is higher: over class B, reported, and
  eco is the mode that meets the class (ADR-0011 3.5).
