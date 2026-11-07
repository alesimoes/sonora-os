# Multiband compressor: crossover design

- Date: 2026-11-06
- Author: dsp-engineer (recovered from the uncommitted worktree backup
  `origin/backup/2026-11-06/multiband`)
- Scope: `plugins/native/multiband` ("Sonora Multiband",
  `org.sonora.multiband`, class B) and `common/dsp/include/sonora/dsp/crossover.h`.
  ADR-0011 §1.12 and §3.2. Not a clone of any unit or plugin (ADR-0004).

## Band count

Three or four bands (a choice parameter, applied at `prepare()` because it
changes buffers and the linear-phase latency). Three bands are the usual
low / mid / high split of a mix-bus or mastering job; the fourth splits the
mid so a vocal or snare range can be held on its own. More bands buy
little for stock use and cost one more crossover and compressor each; the
cap `kMaxCrossoverBands = 4` keeps every buffer sized at `prepare`.
Crossover frequencies are kept ascending, at least 1.25 apart and below
0.45 fs (`order_crossovers`).

## Minimum phase: Linkwitz-Riley, fourth order

S. H. Linkwitz, "Active crossover networks for noncoincident drivers",
JAES 24(1), 1976. Each leg is two cascaded Butterworth sections, built as
trapezoidal state-variable filters with the bilinear transform prewarped at
the crossover, so each leg is -6.02 dB there (the bilinear cramping is above
the crossover and does not affect the sum).

Why order 4 (LR4, 24 dB/oct) and not LR2 or LR8:
- LR2 has the legs in antiphase at the crossover, so the sum needs one leg
  inverted and the polar response has a notch; LR4 legs are in phase.
- LR4: LP + HP = D(-s) / D(s), an exact second-order all-pass of the
  Butterworth denominator D. The sum is flat in magnitude to rounding.
- 24 dB/oct keeps each band's compressor from seeing much of its neighbour
  (the stopband bar is -40 dB a decade out of band) at two biquads per leg.

Four bands are a cascade: each crossover splits the previous high leg, and
the bands already split are run through that crossover's all-pass, so every
band carries the same phase and the sum of all bands is an all-pass of the
input: flat in magnitude, no buffering latency. Its phase delay is not
reported as latency: a host that compensates reported latency would
otherwise shift the whole signal for a pure phase effect.

## Linear phase: windowed-sinc FIR

Kaiser window (J. F. Kaiser, 1974), beta 6 (about 60 dB of stopband),
2049 taps to 48 kHz and 4097 above, odd so the delay is a whole
(taps - 1) / 2 samples: 1024 or 2048 samples of reported latency. Band k is
LP(k) - LP(k-1) and the top band is the input delayed by the same amount
minus the last low-pass, a telescoping sum: the bands sum to the delayed
input exactly whatever the window does to the slopes. The legs are -6 dB at
the crossover (the FIR is symmetric), not -6.02 dB. Convolution is
partitioned (partition 128, head direct) so the FFT's delay is hidden and
the latency is the FIR centre alone; every buffer and plan is sized in
`prepare`. An optional look-ahead of 0 to 10 ms delays the audio, not the
detectors, and is added to the reported latency in either mode.

## Compressors

Each band is the N0 log-domain feed-forward design: Giannoulis, Massberg and
Reiss, "Digital dynamic range compressor design", JAES 60(6), 2012
(quadratic soft knee, branching peak detector), with threshold, ratio,
attack, release, make-up, solo and bypass per band, an optional side-chain
high-pass, auto make-up and a stereo link. Feed-forward for the reason given
in `native-mixbus-n2.md`: a feedback detector cannot hold the displayed
ratio and times.

## Bars and measured figures

The bars are in `plugins/native/multiband/quality.json` (null sum -120 dB,
minimum-phase flatness 0.01 dB, crossover levels, static curves 0.1 dB,
attack/release 10 %, latency equal to reported, zipper -60 dBFS, golden at
44.1, 48, 96 kHz). The figures from the run for this PR are in the PR body
(commands, machine, times). CPU class B (§3.5: p99.9 60 us, max 150 us):
minimum phase is held to the class; linear phase (six partitioned
convolutions) is reported but over class B on the container machine, as the
tape's standard and high qualities are, so it is not held (see the note in
`quality.json`); see the PR for the new figures.
