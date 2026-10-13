# Native plugin suite: published techniques, AI parts, licensing

Owner request, 2026-10-13, with additions the same day: ship our own suite
(EQ, reverb, delay, chorus, compressor, gate, limiter, tube drive, tape,
mix-bus processor, mastering console with AI analysis, opto and FET style
levellers, guitar amps and cabinets, per-instrument tone matching, an AI vocal
tuner, an AI mixing and mastering assistant). This note says, for each, which
published technique to build on, what to read, what it costs and how to prove
it works. It does not choose an architecture; the decisions it forces are
listed at the end and need ADRs.

## Ground rules this note works under

- ADR-0004 (`brain/decisions/ADR-0004-no-proprietary-reverse-engineering.md`)
  and the coordinator's constraint: no copying, decompiling or black-box
  cloning of Avid's, UAD's or anyone's **plugin**. We model **original
  hardware** from public schematics, datasheets and the academic literature,
  and implement console processing from published DSP literature. Training a
  network on a commercial plugin's output is black-box cloning of that plugin
  and is out of scope.
- The repository is **GPL-3.0-or-later** (`LICENSE`). Code we reuse must be
  GPL-3-compatible: MIT, BSD, Apache-2.0, LGPL, GPL-2.0-or-later and GPL-3 are;
  GPL-2.0-only is not; any **NonCommercial** licence (CC BY-NC, BY-NC-SA,
  research-only licences) is not, because GPL-3 §10/§12 forbid adding a
  restriction on commercial redistribution.
- Nothing in `plugins/` exists yet. ADR-0007 places a plugin host at
  `plugins/host/` (Phase 4); first-party DSP has no module yet (see
  "Decisions this forces").
- **Nothing in this note was measured.** Every CPU figure is an estimate from
  operation counts and published claims, marked as such. Per
  `brain/specs/plugin-latency-compensation.md`, no per-plugin budget has been
  measured on any hardware in this project, the UMC1820 included.

Three kinds of claim are kept apart throughout: **Spec/literature** (what the
paper or standard says), **Implementations** (what existing code does, with its
licence), **Measured here** (none yet; listed as open work).

## Cost classes used below (estimates, not measurements)

At 48 kHz and quantum 128 the block is 2.67 ms. Classes are per stereo instance
per block, on one modern x86-64 core, estimated from multiply-add counts:

| Class | Per block | Typical content |
|---|---|---|
| A | < 10 µs | a few biquads, a feed-forward compressor, a gain |
| B | 10–100 µs | 2–4x oversampled static nonlinearity, FDN reverb, modulated delays |
| C | 100–500 µs | 8x oversampled circuit model with Newton iterations, small neural net per sample, short partitioned convolution |
| D | > 500 µs, or not block-causal | long convolution at zero latency, large neural nets, anything needing seconds of look-ahead: analysis pass or worker thread only |

The 72-instance budget argument in `plugin-latency-compensation.md` means
class C must be the exception on a track, not the rule.

---

## 1. Per-plugin techniques

### 1.1 EQ (parametric, shelves, filters)

- **Technique.** Biquad sections. Coefficients from the RBJ Audio EQ Cookbook
  (R. Bristow-Johnson; W3C Audio WG Note,
  https://www.w3.org/TR/audio-eq-cookbook/). The bilinear transform cramps
  bells and shelves near Nyquist; at 48 kHz a 12 kHz bell is visibly narrowed.
  Fix with **matched second-order filters**: M. Vicanek, "Matched Second Order
  Digital Filters", 14 Feb 2016, https://www.vicanek.de/articles/BiquadFits.pdf
  (closed form, cheap enough for per-sample modulation), or Orfanidis, "Digital
  parametric equalizer design with prescribed Nyquist-frequency gain", JAES
  45(6), 1997. For automation without zipper noise use a topology-preserving
  SVF: V. Zavalishin, *The Art of VA Filter Design*, rev. 2.1.2 (2020), and
  A. Simper's Cytomic SVF notes (cytomic.com technical papers).
- **Linear-phase mode** (mastering): FIR designed from the same magnitude
  target, run as partitioned FFT convolution; costs latency (report it to PDC)
  and pre-ringing.
- **Parameters.** Per band: type (bell, low/high shelf, LP/HP 6–48 dB/oct,
  notch, band-pass), frequency 20 Hz–20 kHz, gain ±18–24 dB, Q (or bandwidth
  in octaves; shelves with slope), on/off; output gain; M/S or L/R processing
  on the master; analyser overlay.
- **Aliasing.** Linear, no oversampling. Only the Nyquist warping above.
- **Cost.** A (8 bands ≈ 8 × 5 MACs × 128 × 2). Linear-phase: B–C plus latency.
- **Validation.** Magnitude/phase against the analog prototype computed in
  double precision (the cookbook's s-domain forms): error < 0.1 dB to 10 kHz,
  report the error above. Null test: float32 RT path vs a float64 offline
  reference of the same coefficients, residual below −120 dBFS for a sweep.
  Modulation test: sweep the frequency at audio rate, check no instability
  (SVF) and bounded transients.
- **Implementations to learn from.** x42 `fil4.lv2` (GPL-2.0-or-later, header
  checked), Ardour `a-eq` (GPL-2.0-or-later), LSP Plugins EQs (LGPL-3.0).

### 1.2 Reverb

- **Technique.** Algorithmic first: feedback delay networks (J.-M. Jot and
  A. Chaigne, "Digital delay networks for designing artificial reverberators",
  AES 90th Convention, 1991, preprint 3030; S. Schlecht and E. Habets, "On
  lossless feedback delay networks", IEEE Trans. Signal Process. 65(6), 2017)
  and the plate topology in J. Dattorro, "Effect Design Part 1: Reverberator
  and Other Filters", JAES 45(9), 1997, pp. 660–684. Survey: V. Välimäki,
  J. Parker, L. Savioja, J. O. Smith, J. Abel, "Fifty years of artificial
  reverberation", IEEE TASLP 20(5), 2012. Historical: M. Schroeder, "Natural
  sounding artificial reverberation", JAES 10(3), 1962.
- **Convolution** (rooms, plates, springs we capture ourselves): uniformly or
  non-uniformly partitioned convolution — W. Gardner, "Efficient convolution
  without input-output delay", JAES 43(3), 1995; F. Wefers, *Partitioned
  convolution algorithms for real-time auralization*, PhD, RWTH Aachen, 2015.
  IR capture by exponential sweep (A. Farina, "Simultaneous measurement of
  impulse response and distortion with a swept-sine technique", AES 108th
  Convention, 2000).
- **Parameters.** Pre-delay, decay (RT60) with low/high multipliers or
  crossovers, size, diffusion/density, early/late balance, modulation depth and
  rate, damping, low/high cut on the wet path, width, mix, freeze.
- **Aliasing.** Linear. Modulated delays need good fractional interpolation
  (see 1.3).
- **Cost.** FDN (8–16 lines): B. Dattorro plate: A–B. Convolution: a 3 s stereo
  IR at zero latency is D; with 128-sample first partition and growing
  partitions it is C, and belongs on a send, not per track.
- **Validation.** RT60 per octave band from the impulse response by Schroeder
  backward integration, per ISO 3382-1:2009; compare to the set decay. Echo
  density and modal density against the FDN design. Stability: lossless
  matrix check (Schlecht & Habets) at decay = ∞ must not grow. Listening on
  impulsive material (snare, clicks) for flutter and metallic ringing.
- **Implementations.** Dragonfly Reverb (GPL-3.0; built on Freeverb3,
  GPL-3), Ardour `a-reverb` (GPL-2.0-or-later), Airwindows reverbs (MIT),
  LSP impulse reverb (LGPL-3.0).
- **Not allowed.** Shipping IRs sampled from commercial reverbs or from other
  people's IR libraries without a licence. We capture our own (rooms we rent,
  a plate or spring unit we own) — ADR-0004 lists impulse responses
  explicitly.

### 1.3 Delay and chorus (and tape/BBD flavours)

- **Technique.** Circular buffers with fractional delay: Lagrange or allpass
  interpolation (T. Laakso, V. Välimäki, M. Karjalainen, U. Laine, "Splitting
  the unit delay", IEEE Signal Processing Magazine 13(1), 1996); modulation
  and chorus/flanger topologies in J. Dattorro, "Effect Design Part 2:
  Delay-Line Modulation and Chorus", JAES 45(10), 1997. Textbook: U. Zölzer
  (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011. Analog flavours:
  bucket-brigade — C. Raffel and J. O. Smith, "Practical modeling of
  bucket-brigade device circuits", DAFx-10, 2010; tape echo — S. Arnardottir,
  J. Abel, J. O. Smith, "A digital model of the Echoplex tape delay", AES
  125th Convention, 2008.
- **Parameters.** Time (ms or tempo-synced note values, L/R independent),
  feedback, ping-pong, filters in the feedback loop, modulation rate/depth,
  saturation in the loop (analog mode), ducking, mix. Chorus: voices, rate,
  depth, delay, spread, mix.
- **Aliasing.** Interpolation error only; saturation in a feedback loop needs
  ADAA or 2x (see 1.8).
- **Cost.** A (digital), B (BBD/tape modes).
- **Validation.** Impulse delay exact to the sample at integer settings;
  sweep the modulated delay and measure interpolation noise floor; feedback at
  0.99 must stay bounded with saturation on.
- **Implementations.** Ardour `a-delay` (GPL-2.0-or-later), Airwindows (MIT),
  Surge XT effects (GPL-3.0).

### 1.4 Compressor, gate, expander

- **Technique.** D. Giannoulis, M. Massberg, J. Reiss, "Digital dynamic range
  compressor design — a tutorial and analysis", JAES 60(6), 2012, pp. 399–408:
  log-domain gain computer with soft knee, smooth branching peak detector,
  feed-forward vs feedback topology and their audible difference. Also
  G. McNally, "Dynamic range control of digital audio signals", JAES 32(5),
  1984; J. Reiss and A. McPherson, *Audio Effects: Theory, Implementation and
  Application*, CRC, 2014, ch. 6. Gate: same detector, hysteresis (separate
  open/close thresholds), hold, range.
- **Parameters.** Threshold, ratio, knee, attack, release (with auto/program
  dependent option), make-up (with auto), detector RMS/peak, side-chain input
  and side-chain HPF/EQ, look-ahead, mix (parallel), stereo link, gain
  reduction meter. Gate: threshold, range, attack, hold, release, hysteresis,
  key filter.
- **Aliasing.** The gain signal modulates the audio; fast attack plus heavy
  ratio produces sidebands that can alias. 2x oversampling as an option.
- **Cost.** A; B with oversampling.
- **Validation.** Static curve (input level vs output level) with stepped sine
  must match the formula within 0.1 dB; attack/release measured on tone bursts
  (time to 63 %) against the set values; null test of the parallel mix at 0 %
  and 100 %.
- **Implementations.** Ardour `a-comp` (GPL-2.0-or-later), ZamComp/ZamGate
  (GPL-2.0-or-later), LSP compressors and gates (LGPL-3.0), Calf (LGPL-2.1).

### 1.5 Opto leveller (in the spirit of the 1960s optical levelling amplifier)

Our name, not theirs (see §5). The reference circuit is the Teletronix LA-2A.

- **What the public record says.** UA's reissue manual (Universal Audio,
  *Teletronix LA-2A Leveling Amplifier* manual, © 2000, copy at
  https://www.barryrudolph.com/recall/manuals/uaaudiola2.pdf) prints the input
  and gain-reduction schematic (Fig. 5), side-chain (Fig. 6) and output stage
  (Fig. 7) and describes the T4 cell: an electroluminescent panel driving a
  photoresistor; release "quickly (40–80 milliseconds) to approximately half
  of its off resistance", the rest over "as much as several seconds", and
  slower release after heavier, longer reduction (program-dependent memory).
  Its spec list: release ≈ 0.06 s for 50 %, 0.5–5 s for complete release,
  distortion < 0.35 % THD at +10 dBm. That manual text is copyrighted; the
  circuit facts and figures are what we use, and we re-enter the circuit as
  our own netlist rather than redistributing the drawing.
- **Technique.** Split the model: (a) the tube gain and output stages by
  circuit simulation (1.8, WDF or nodal DK); (b) the T4 cell as a
  light-to-resistance dynamic model. Literature for (b): J. Najnudel,
  R. Müller, T. Hélie, D. Roze, "Power-balanced dynamic modeling of vactrols:
  application to a VTL5C3/2", DAFx-23, 2023 (physical, port-Hamiltonian,
  program-dependent); F. Eichas and U. Zölzer, "Modeling of an
  optocoupler-based audio dynamic range control circuit", Proc. SPIE 9948,
  2016 (gray-box, identified by iterative error minimisation); the
  Perkin-Elmer/Excelitas VTL5C datasheets for LDR time constants. An EL-panel
  plus CdS cell is not a VTL5C (LED plus LDR); the EL panel's response must be
  measured or taken from literature on the T4 specifically — **open**.
- **Hardware-measured data, public.** SignalTrain LA-2A dataset, Zenodo record
  3824876, v1.1 (2020), **CC BY 4.0** (record checked 2026-10-13), recorded
  from a real Teletronix LA-2A at several Peak Reduction settings, 20.7 GB.
  Use: validation of our circuit model. Neural black-box models trained on it
  exist (C. Steinmetz and J. Reiss, "Efficient neural networks for real-time
  modeling of analog dynamic range compression", AES 152nd Convention, 2022,
  arXiv:2102.06200; code micro-TCN, Apache-2.0). Whether a black-box model of
  the *hardware* is within the coordinator's "model from schematics and
  literature" rule is a coordinator decision; this note recommends using the
  data to validate, not to train the shipped model.
- **Parameters.** Peak Reduction, Gain, Compress/Limit switch, side-chain
  emphasis (the manual's R37 HF emphasis), meter (GR / output), mix.
- **Aliasing.** The tube stages generate harmonics: 2–4x oversampling or ADAA
  on the nonlinear parts. The T4 model itself is slow and needs none.
- **Cost.** B (detector and cell are cheap; the tube stages dominate).
- **Validation.** Release curve after 1, 5, 20 s of 10 dB reduction must show
  the two-stage release and the history dependence the manual describes;
  error metrics against SignalTrain recordings (ESR, gain-reduction envelope
  error) at the dataset's settings; THD at +10 dBm vs the manual's figure;
  blind listening against SignalTrain audio.

### 1.6 FET limiter (in the spirit of the FET limiting amplifier)

Reference circuit: UREI/UA 1176LN.

- **What the public record says.** UA's published specs: attack 20–800 µs,
  release 50 ms–1.1 s (UA support page,
  https://help.uaudio.com/hc/en-us/articles/206007816-1176LN-Classic-Limiting-Amplifier).
  Ratios 4, 8, 12, 20 and the all-buttons mode are documented in the UA
  manual (https://c3.zzounds.com/media/1176LN_manual-1a90c9dff91ec30316551923e877c4d0.pdf;
  the server refused our fetch, ratios **not re-verified** here).
- **Technique.** Nodal DK / state-space circuit simulation of the FET
  voltage-divider gain cell, its feedback side-chain and the class-A output
  stage: D. Yeh, J. Abel, J. O. Smith, "Automated physical modeling of
  nonlinear audio circuits for real-time audio effects — Part I", IEEE TASLP
  18(4), 2010; M. Holters and U. Zölzer, "A generalized method for the
  derivation of non-linear state-space models from circuit schematics",
  EUSIPCO 2015. Applied to this unit: H. A. M. M. Antão, *Modelação física do
  compressor áudio UREI 1176LN*, MSc thesis, Universidade de Aveiro / TU
  Hamburg, 2014 (https://ria.ua.pt/bitstream/10773/12791/1/tese.pdf; we read
  the abstract only); gray-box block model: E. Gerat, F. Eichas, U. Zölzer,
  "Virtual analog modeling of a UREI 1176LN dynamic range control system",
  AES 143rd Convention, 2017, paper 9852 (abstract only; paywalled).
- **Parameters.** Input, Output, Attack, Release (note the original's knob
  direction: fastest at clockwise end), Ratio 4/8/12/20 and all-in, meter
  select, mix, side-chain HPF (our addition, labelled as such).
- **Aliasing.** FET distortion rises with gain reduction and the attack is
  sub-millisecond: 4–8x oversampling or a smaller factor plus ADAA. The
  feedback detector makes the model implicit: Newton per sample.
- **Cost.** C at 4x with Newton iteration; B if the gain cell is reduced to a
  fitted static nonlinearity plus a linear detector (Gerat et al. style).
- **Validation.** Attack/release times against the published ranges, THD vs
  gain reduction curve (must rise with GR), all-in mode's characteristic
  over-compression on tone bursts. Hardware recordings of a unit we own or
  rent are needed — no CC dataset of this unit was found — **open**.

### 1.7 Limiter (true-peak) and the loudness meter

- **Spec.** ITU-R BS.1770-5 (11/2023), Annex 1: K-weighting, 400 ms gating
  blocks with 75 % overlap, absolute gate −70 LKFS, relative gate −10 LU;
  Annex 2: true-peak = attenuate (12.04 dB, unnecessary in float), **4x
  oversample 48 → 192 kHz**, low-pass, absolute value; "higher sampling rates
  and over-sampling ratios are preferred"; at 96 kHz 2x suffices; it prints an
  example 48-tap, 4-phase FIR (read from the PDF, 2026-10-13). EBU R 128
  (v5.0, November 2023, https://tech.ebu.ch/docs/r/r128v5_0.pdf): target
  −23.0 LUFS, true peak not to exceed −1 dBTP during production; meter per
  EBU Tech 3341 (momentary, short-term, integrated), LRA per EBU Tech 3342.
- **Limiter technique.** Look-ahead peak limiter: delay the audio by the
  look-ahead, compute the required gain on the oversampled signal, take a
  running minimum over the look-ahead window, smooth with attack equal to the
  window so the gain is reached before the peak (Zölzer, *DAFX*, 2nd ed.,
  ch. 4; Reiss & McPherson ch. 6). The true-peak detector must use the same
  oversampling as the meter or the limiter will pass overs the meter flags.
- **Parameters.** Ceiling (dBTP), input gain or threshold, release (with
  auto), look-ahead, stereo link, true-peak on/off, oversampling factor,
  dither and noise shaping on output (R. Lipshitz, R. Wannamaker,
  J. Vanderkooy, "Quantization and dither: a theoretical survey", JAES 40(5),
  1992; M. Gerzon and P. Craven, "Optimal noise shaping and dither of digital
  signals", AES 87th Convention, 1989).
- **Cost.** B (4x polyphase FIR + sliding minimum). Latency = look-ahead +
  filter delay, reported to PDC.
- **Validation.** EBU Tech 3341 and 3342 publish test signals with expected
  readings — the meter must pass all of them. Limiter: no output sample of the
  4x-oversampled output above the ceiling over a corpus that includes
  intersample-peak test cases (e.g. fs/4 sine at 45° phase); compare our
  meter with libebur128 (MIT) on the same files.
- **Implementations.** libebur128 (MIT), x42 `dpl.lv2` look-ahead limiter
  (GPL-3.0), x42 `meters.lv2` (GPL-2.0 per GitHub metadata; check headers),
  ZaMaximX2 (GPL-2.0-or-later), pyloudnorm (MIT, Python, for test oracles).

### 1.8 Tube drive (and the vacuum-tube stages inside 1.5, 1.6, 1.11)

- **Technique.** Triode models: N. Koren, "Improved VT models for SPICE
  simulations", Glass Audio 5, 1996 (normankoren.com); K. Dempwolf and
  U. Zölzer, "A physically-motivated triode model for circuit simulations",
  DAFx-11, 2011; I. Cohen and T. Hélie, "Measures and parameter estimation of
  triodes, for the real-time simulation of a multi-stage guitar preamplifier",
  AES 129th Convention, 2010. Circuit solvers: wave digital filters
  (A. Fettweis, "Wave digital filters: theory and practice", Proc. IEEE 74(2),
  1986; M. Karjalainen and J. Pakarinen, "Wave digital simulation of a
  vacuum-tube amplifier", ICASSP 2006; K. Werner, *Virtual analog modeling of
  audio circuitry using wave digital filters*, PhD, Stanford, 2016) or nodal
  DK (Yeh et al. 2010). Review: J. Pakarinen and D. Yeh, "A review of digital
  techniques for modeling vacuum-tube guitar amplifiers", Computer Music
  Journal 33(2), 2009; D. Yeh, *Digital implementation of musical distortion
  circuits by analysis and simulation*, PhD, Stanford, 2009.
- **Harmonic profile.** A single-ended triode stage gives predominantly even
  (2nd) harmonics, push-pull cancels even order — a consequence of the
  circuit, which a circuit model reproduces without being told to. Measure
  harmonic profiles with the synchronized swept-sine method: A. Novák,
  L. Simon, P. Lotton, "Synchronized swept-sine: theory, application, and
  implementation", JAES 63(10), 2015 (separates each harmonic's response from
  one sweep).
- **Noise.** Tube noise is shot and 1/f (flicker) noise at the grid; model as
  shaped noise at the stage input, level from the tube datasheet's equivalent
  noise resistance where given. Off by default; a parameter, not a
  side-effect.
- **Parameters.** Drive, tube type/bias (selects fitted triode parameters),
  tone (tilt or a modelled tone stack), output, mix, oversampling quality.
- **Aliasing.** The strongest need in the suite. Options: 4–8x oversampling
  with polyphase half-band filters; antiderivative antialiasing for
  memoryless stages (J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the
  aliasing of nonlinear waveshaping using continuous-time convolution",
  DAFx-16; S. Bilbao, F. Esqueda, J. Parker, V. Välimäki, "Antiderivative
  antialiasing for memoryless nonlinearities", IEEE SPL 24(7), 2017). ADAA
  does not apply directly to stateful (capacitor-coupled) stages; there,
  oversample.
- **Cost.** B (static waveshaper + 4x) to C (WDF with a Newton-solved triode
  at 8x).
- **Validation.** Plate characteristic curves of the fitted model vs the
  tube datasheet curves (RCA/GE 12AX7 datasheets); harmonic levels vs SPICE
  simulation of the same netlist with Koren models (ngspice, BSD); aliasing:
  a 5–15 kHz sine sweep must show alias products below a stated floor
  (−90 dBFS proposed) in the 0–20 kHz band.
- **Implementations.** ZamTube (WDF triode, GPL-2.0-or-later, header checked),
  chowdsp_wdf (BSD-3-Clause), ACME.jl (MIT, Julia, nodal state-space
  generator from netlists — useful offline to derive matrices), Guitarix
  (GPL, per-file headers to check).

### 1.9 Transformers, mains hum, noise floors (owner addition)

- **Transformer.** Three behaviours: LF loss from finite primary inductance
  (a high-pass whose corner falls with source impedance), HF resonance from
  leakage inductance and winding capacitance, and core saturation with
  hysteresis that grows as frequency falls (flux ∝ V/f) — which is why
  transformer distortion is a low-frequency, level-dependent effect. Model the
  core with Jiles–Atherton hysteresis (D. Jiles and D. Atherton, "Theory of
  ferromagnetic hysteresis", J. Magn. Magn. Mater. 61, 1986, pp. 48–60)
  inside the circuit solver: M. Holters and U. Zölzer, "Circuit simulation
  with inductors and transformers based on the Jiles–Atherton model of
  magnetization", DAFx-16, 2016. Background on audio transformers:
  B. Whitlock, "Audio transformers", in G. Ballou (ed.), *Handbook for Sound
  Engineers* (Focal Press). Validation: THD vs frequency at fixed level must
  rise toward low frequencies; frequency response against the
  manufacturer's datasheet curve for the named transformer (e.g. the output
  transformer on a schematic); B–H loop of the model vs datasheet core
  material.
- **Mains hum.** Not a circuit effect to simulate but a defect to optionally
  add: a 50 or 60 Hz fundamental with harmonics; full-wave rectifier ripple
  at 100/120 Hz with the buzzy, odd-and-even spectrum of rectifier charging
  pulses. Generated by an oscillator whose harmonic amplitudes come from
  measurements of real units (ours) — no paper models this better than a
  measurement does. Default off. Validation: spectrum vs the measured unit.
- **Tape hiss and noise floors.** Model as stationary noise shaped to the
  measured or published spectrum, passing through the same playback
  equalisation as the signal (so hiss is shaped by the de-emphasis), and
  modulated slightly by the signal (bias-noise modulation). Level references:
  manufacturer S/N specs for the machine and tape formulation, measured per
  IEC/CCIR weighting (ITU-R BS.468-4). Validation: weighted noise level and
  spectrum shape vs the published S/N figure.
- **Implementations.** ChowTapeModel (GPL-3.0) includes hysteresis; chowdsp
  modules have Jiles–Atherton code (licences per module, check before use).

### 1.10 Tape machine simulator

- **Technique.** J. Chowdhury, "Real-time physical modelling for analog tape
  machines", DAFx-19, 2019 (record head with Jiles–Atherton hysteresis and
  bias, playback head losses, wow and flutter; implemented as CHOW Tape,
  GPL-3.0, repo `jatinchowdhury18/AnalogTapeModel`, README marks it
  deprecated). Playback losses: spacing, thickness and gap loss from
  R. Wallace, "The reproduction of magnetically recorded signals", Bell System
  Technical Journal 30(4), 1951; H. N. Bertram, *Theory of Magnetic
  Recording*, Cambridge University Press, 1994. Record/playback equalisation
  time constants per the IEC 60094 series and NAB. Wow and flutter: a
  low-frequency (wow) plus higher-rate (flutter) modulation of a fractional
  delay, amplitudes calibrated to weighted peak flutter measured per AES6-2008
  / IEC 60386. Head bump: LF resonance from head geometry (Bertram).
- **Parameters.** Machine/tape type (selects EQ standard, speed 7.5/15/30 ips,
  head geometry), input level (drive into hysteresis), bias, wow, flutter,
  hiss, head bump, output, auto-gain compensation.
- **Aliasing.** Hysteresis is strongly nonlinear and stateful: Chowdhury runs
  it oversampled (2–16x selectable in CHOW Tape). Plan 4x default.
- **Cost.** C at 4x with an implicit hysteresis solver; B in a reduced mode.
- **Validation.** Frequency response at −20 dB and 0 dB relative to reference
  fluxivity (the classic HF compression at high level); MOL/3 % THD point vs
  published tape formulation data sheets; flutter measured by AES6 method on
  a 3.15 kHz tone; hysteresis loop plots vs Jiles–Atherton reference
  integration in float64 (null test of our RT solver against our offline
  solver).

### 1.11 Guitar amp heads and speaker cabinets (owner addition)

- **Two families.** (a) White-box circuit models of preamp, tone stack, power
  amp and output transformer (1.8, 1.9; tone stacks: D. Yeh and J. O. Smith,
  "Discretization of the '59 Fender Bassman tone stack", DAFx-06). (b)
  Black-box neural capture of **a unit we own or rent**: WaveNet-style
  (E.-P. Damskägg, L. Juvela, E. Thuillier, V. Välimäki, "Deep learning for
  tube amplifier emulation", ICASSP 2019), LSTM/GRU (A. Wright,
  E.-P. Damskägg, V. Välimäki, "Real-time black-box modelling with recurrent
  neural networks", DAFx-19; A. Wright et al., "Real-time guitar amplifier
  emulation with deep learning", Applied Sciences 10(3), 2020). Neural Amp
  Modeler is the widely used open implementation of the WaveNet/LSTM
  approach: `sdatkinson/neural-amp-modeler` (trainer, **MIT**),
  `NeuralAmpModelerCore` (**MIT**), `NeuralAmpModelerPlugin` (**MIT**),
  `neural-amp-modeler-lv2` (GPL-3.0). Others: GuitarML Proteus (GPL-3.0),
  GuitarLSTM (GPL-3.0), Automated-GuitarAmpModelling (GPL-3.0), AIDA-X
  (GPL-3.0). RT inference library: RTNeural (BSD-3-Clause; J. Chowdhury,
  "RTNeural: fast neural inferencing for real-time systems",
  arXiv:2106.03037, 2021).
- **Cabinets.** Linear part: cabinet + speaker + mic as an impulse response,
  captured by Farina sweep through a reamp box at moderate level; convolution
  (1.2) with a short IR (≤ 200 ms) is class B. Speaker nonlinearity at high
  level (cone excursion, voice-coil heating) is real but secondary; Thiele–
  Small parameters from the speaker datasheet give the LF behaviour.
- **How captures are made legally.** Our own captures of hardware we own,
  rent or borrow with permission; our own room and microphones. A capture is
  a measurement of a physical device's behaviour; the device maker's
  trademark stays out of the name. Not allowed: copying other people's NAM
  captures, cab IRs, Kemper/ToneX profiles or IR packs unless each carries a
  licence that permits redistribution in a GPL product (most community
  captures carry no licence at all — treat as all rights reserved).
- **"Artist tone".** Artist names cannot appear in product or preset names
  (trademark and right of publicity, §5). The route to "sounds like the
  record" is reference matching from audio the **user** supplies (§2.2):
  we ship generic amp/cab captures, the user drops in a reference, the
  matcher picks amp, cab and EQ and drives toward it. Nothing named after an
  artist ships.
- **Parameters.** Amp: gain, bass/mid/treble/presence, master, channel, input
  trim calibrated to dBu (captures are level-dependent). Cab: IR select, mic
  position blend, low/high cut. Model quality vs CPU switch.
- **Aliasing.** Neural models trained at 48 kHz learn the band-limited
  response of the capture; they still alias if driven outside training range.
  Circuit models: 4–8x.
- **Cost.** NAM-class WaveNet "standard" models are C (published as running
  live on desktop CPUs; not measured here); LSTM with 16–32 hidden units is
  B–C; circuit model C.
- **Validation.** ESR (error-to-signal ratio, as used by Wright et al.)
  against held-out reamped DI from the same unit; blind ABX against the
  hardware recorded through the same chain; aliasing sweep as in 1.8.

### 1.12 Mix-bus processor

- **Technique.** A composition of the above: gentle feed-forward or feedback
  VCA-style compressor (Giannoulis et al. 2012, the feedback topology for
  "glue"), low-ratio, slow attack, auto-release; transformer/tube colour (1.8,
  1.9); tilt/shelf EQ (1.1); stereo width (M/S gain). The classic console bus
  compressors are VCA designs; there is no single published paper on them to
  cite, so the claim is functional, not a clone. Bob Katz, *Mastering Audio*,
  3rd ed., Focal Press, 2015, and the mixing texts in §2.4 describe the
  practice.
- **Parameters.** Threshold, ratio (1.5/2/4/10), attack, release incl. auto,
  side-chain HPF, make-up, mix, colour amount, width.
- **Cost.** B. Validation as 1.4 plus 1.8.

### 1.13 Mastering console

- **Chain.** Input trim → linear-phase or minimum-phase EQ (1.1) → M/S
  multiband compression (Linkwitz–Riley crossovers summed flat; validate the
  sum by null test with all bands at unity) → colour (tube/tape, optional) →
  stereo width with mono-compatibility check → true-peak limiter (1.7) →
  dither (1.7). Meters: BS.1770 integrated/short-term/momentary, LRA (Tech
  3342), true peak, PLR, correlation, spectrum.
- **Cost.** B–C, plus latency (linear-phase + look-ahead) reported to PDC.
  On the master bus only, so one instance.
- **AI analysis** in §2.1.

### 1.14 Vocal tuner

- **Technique.** Pitch tracking (YIN: A. de Cheveigné and H. Kawahara, JASA
  111(4), 2002; pYIN: M. Mauch and S. Dixon, ICASSP 2014; CREPE: J. W. Kim,
  J. Salamon, P. Li, J. P. Bello, ICASSP 2018), target note from key/scale
  (§2.3), then pitch shift preserving formants: TD-PSOLA (E. Moulines and
  F. Charpentier, Speech Communication 9(5–6), 1990) or resampling with
  period-synchronous crossfades for real time; phase vocoder with phase
  locking (J. Laroche and M. Dolson, IEEE TSAP 7(3), 1999) offline.
  The original autocorrelation-and-resample auto-tune patent, US 5,973,252
  (Hildebrand, filed 1998), is listed by Google Patents as "Expired —
  Lifetime"; that status is Google's assumption, not a legal finding.
- **Parameters.** Key, scale (or "detect"), retune speed, humanize, formant
  preserve, note enable/bypass per scale degree, transpose, MIDI target
  input, correction amount.
- **Latency (literature arithmetic, not measured).** Detecting 80 Hz needs at
  least two periods (25 ms) of signal; a block is 2.67 ms, so a real-time
  tuner must buffer and report ~10–30 ms to PDC. Fine for playback and mix,
  audible when tracking through it. CREPE's input is 1024 samples at 16 kHz
  (64 ms, read from the paper 2026-10-13): unusable as a live-monitoring
  tracker; usable in an analysis pass.
- **Implementations.** zita-at1 / x42 `fat1.lv2` (GPL-2.0-or-later, header of
  `retuner.cc` checked; autocorrelation + resampler, reports latency), pYIN
  (GPL-2.0-or-later, `c4dm/pyin`), aubio (GPL-3.0), Rubber Band (GPL-2.0-or-
  later or commercial), Signalsmith Stretch (MIT), torchcrepe and CREPE code
  (MIT; **weights see §3**).

---

## 2. The AI parts

### 2.1 Mastering console AI ("what is excessive, what needs cutting")

- **Approach.** An analysis pass, not a black box in the signal path. Compute
  descriptors over the whole programme: loudness and LRA (BS.1770/Tech 3342),
  true peak, crest factor/PLR, long-term spectrum vs fractional-octave-smoothed
  target curves (P. Hatziantoniou and J. Mourjopoulos, "Generalized
  fractional-octave smoothing of audio and acoustic responses", JAES 48(4),
  2000), resonances (narrow peaks above the smoothed spectrum, persistent over
  time), harshness and sibilance bands, low-end mono compatibility (L−R energy
  below ~150 Hz), inter-channel correlation, masking between bands
  (psychoacoustic excitation: B. Moore, B. Glasberg, T. Baer, "A model for the
  prediction of thresholds, loudness, and partial loudness", JAES 45(4),
  1997; ISO 532-2:2017). Map findings to **explicit settings of our own
  console** (an EQ cut at 3.1 kHz, −2 dB, Q 4, with the reason), which the
  engineer accepts or edits. Reference-based: match a user-supplied reference
  track's spectrum, loudness, width and dynamics — the approach of Matchering
  (GPL-3.0, `sergree/matchering`).
- **Target curves.** Derive from our own licensed corpus, not from
  commercial releases we do not have rights to analyse at scale (§3).
- **Real time?** The descriptors are cheap but need the whole programme for
  integrated values: analysis-time. Live meters are real time.

### 2.2 Per-instrument timbre match ("guitar matched to a reference tone")

- **Tier 1, match EQ.** Long-term average spectrum of source and reference,
  ratio, fractional-octave smoothing, regularised, realised as minimum-phase
  FIR (cepstral method, Oppenheim & Schafer, *Discrete-Time Signal
  Processing*) or fitted to a few parametric bands so it stays editable.
  Analysis offline; filter is class A–B in real time.
- **Tier 2, conditioned nonlinearity.** Choose and set amp/cab captures (1.11)
  and drive by optimisation against the reference: differentiable DSP
  (J. Engel, L. Hantrakul, C. Gu, A. Roberts, "DDSP: Differentiable digital
  signal processing", ICLR 2020; code Apache-2.0) and audio-effect style
  transfer (C. Steinmetz, N. Bryan, J. Reiss, "Style transfer of audio effects
  with differentiable signal processing", JAES 70(9), 2022 — its code
  DeepAFx-ST is under the Adobe Research License, **noncommercial research
  only, incompatible**; the paper's method is usable, the code is not).
  Optimisation runs offline (seconds to minutes); the result is a set of
  plugin parameters played back in real time.
- **Tier 3, neural timbre transfer/resynthesis** (DDSP autoencoder, RAVE).
  RAVE's LICENSE is **CC BY-NC 4.0** (repo `acids-ircam/RAVE`, checked) —
  incompatible. DDSP-style resynthesis replaces the performance with a
  synthesised one; for guitar this is a different product. Not recommended
  for v1.
- **Separating the instrument from a commercial reference** needs a source
  separator. Demucs code is MIT (`facebookresearch/demucs`), but the weights'
  training data (MUSDB18-HQ, educational use only, plus undisclosed data) make
  the weights' provenance an **open question**; v1 should require an isolated
  reference.

### 2.3 Vocal tuner AI: pitch and key

- **Pitch.** YIN/pYIN in real time on the audio thread (class B, with
  reported latency); CREPE-class networks in an analysis pass for a
  high-quality note map (graphical correction mode).
- **Key/scale.** Chroma/HPCP features (E. Gómez, *Tonal description of music
  audio signals*, PhD, UPF, 2006) correlated with key profiles
  (C. Krumhansl and E. Kessler, Psychological Review 89(4), 1982;
  D. Temperley, Music Perception 17(1), 1999). Runs over the session's
  instrumental tracks as an analysis pass; the result is shown and the user
  confirms. CNN key classifiers (F. Korzeniowski and G. Widmer, EUSIPCO 2017)
  ship in madmom whose **models are CC BY-NC-SA 4.0** (code BSD) and Essentia
  (code **AGPL-3.0**, models non-commercial) — not usable as shipped.

### 2.4 AI mixing and mastering assistant (owner addition)

- **The workflow it reproduces** is published practice, not anyone's
  software: gain staging to a nominal level, bus structure (drums, bass,
  guitars, keys, vocals, FX returns), parallel compression and saturation on
  buses, shared reverbs and delays on sends, a mix-bus chain, stems out to
  mastering. Sources: R. Izhaki, *Mixing Audio*, 3rd ed., Routledge, 2017;
  B. Owsinski, *The Mixing Engineer's Handbook*; M. Senior, *Mixing Secrets
  for the Small Studio*, 2nd ed., 2018; B. Katz, *Mastering Audio*, 3rd ed.
- **Research to build on.** Survey: B. De Man, J. Reiss, R. Stables, "Ten
  years of automatic mixing", 3rd Workshop on Intelligent Music Production,
  2017; B. De Man, R. Stables, J. Reiss, *Intelligent Music Production*,
  Focal Press, 2019. Rule-based: E. Perez-Gonzalez and J. Reiss, "Automatic
  gain and fader control for live mixing", WASPAA 2009; B. De Man and
  J. Reiss, "A knowledge-engineered autonomous mixing system", AES 135th
  Convention, 2013; E. Pestana and J. Reiss, "Intelligent audio production
  strategies informed by best practices", AES 53rd Conference, 2014;
  S. Hafezi and J. Reiss, "Autonomous multitrack equalization based on
  masking reduction", JAES 63(5), 2015. Learned: C. Steinmetz, J. Pons,
  S. Pascual, J. Serrà, "Automatic multitrack mixing with a differentiable
  mixing console of neural audio effects", ICASSP 2021; M. Martínez-Ramírez
  et al., "Automatic music mixing with deep learning and out-of-domain data",
  ISMIR 2022 (code `sony/FxNorm-automix`, MIT); reference-based: J. Koo et
  al., "Music mixing style transfer: a contrastive learning approach to
  disentangle audio effects", ICASSP 2023 (code MIT); S. Vanka et al.,
  "Diff-MST: differentiable mixing style transfer", ISMIR 2024 (repo LICENSE
  **CC BY-NC-SA 4.0**, incompatible). Toolkit: `csteinmetz1/automix-toolkit`
  (Apache-2.0).
- **Design recommended.** The assistant proposes a mix *as a session edit*
  using our plugins and our bus/send graph (`specs/timeline-model.md`): it
  inserts plugins, sets parameters, creates sends and parallel buses, and every
  action is an undoable operation the engineer can inspect. Learned models
  predict parameters of our transparent processors (the differentiable-console
  idea with our DSP made differentiable offline), not audio.
- **Real time vs analysis.** All of it is analysis-time (seconds to minutes
  over the session, on a worker or in a separate process). What plays back is
  ordinary plugin processing. Nothing from §2 needs to run on the audio thread.
- **Evaluation.** Listening tests: MUSHRA (ITU-R BS.1534-3) for intermediate
  quality, ITU-R BS.1116-3 for small impairments; with experienced mixing
  engineers as listeners, against human mixes and an anchor. The Mix
  Evaluation Dataset (B. De Man and J. Reiss, DAFx-17) documents how. Objective
  metrics as diagnostics only: loudness/LRA, spectral balance distance,
  stereo width, multi-resolution STFT distance to a reference mix (used in
  Steinmetz 2021). Objective metrics do not substitute for the listening test.

### 2.5 Reverb and delay suggestions: time, style, amount (owner example)

The owner's case: put reverb and delay on vocals and instruments at the
right time for the song, the right size for the style, and the right amount
so it is not excessive — optionally matching an artist's reference the user
supplies.

**Times from tempo (analysis-time, trivial).**
- Tempo comes from the session tempo map when there is one; otherwise from
  beat tracking over the session (D. Ellis, "Beat tracking by dynamic
  programming", J. New Music Research 36(1), 2007; implemented in librosa,
  ISC licence). Detected tempo is shown and confirmed, never applied silently.
- Arithmetic: quarter note = 60 000 / BPM ms; dotted = ×1.5; triplet = ×2/3;
  halve/double for 1/8, 1/16, 1/2. At 120 BPM: 1/4 = 500 ms, dotted 1/8 =
  375 ms, 1/8 triplet = 166.7 ms. The delay plugin stores the note value, not
  the milliseconds, so a tempo change follows it.
- Reverb tied to tempo: pre-delay as a short note value (1/64–1/32) so the
  wet onset sits behind the dry transient; decay chosen so the tail falls
  ~60 dB by the next beat or bar depending on density of the part. **This is
  practitioner convention from mixing texts** (Owsinski, Izhaki, Senior,
  §2.4), not a measured perceptual result; the note treats it as a starting
  point the user can override.
- Genre/style defaults (e.g. short plates and slap delays vs long halls) are
  a table we author ourselves from those texts and from analysis of our own
  licensed corpus — not from presets of other products.

**Estimating reverb and delay from a user-supplied reference
(analysis-time).**
1. *Isolate the part.* A vocal in a full mix needs source separation.
   Options and their licences (checked 2026-10-13): Demucs (code MIT,
   `facebookresearch/demucs`; Rouard, Massa, Défossez, "Hybrid transformers
   for music source separation", ICASSP 2023), Open-Unmix (code MIT,
   `sigsep/open-unmix-pytorch`; its README says `umxl` weights are **CC
   BY-NC-SA 4.0** — incompatible — and `umxhq` is trained on MUSDB18-HQ,
   educational use only — incompatible), Spleeter (code MIT; the README does
   not document the training data — provenance **open**). Every pretrained
   separator found is tied to MUSDB18 or undocumented data, so the shippable
   option is **retraining on licensed data** or asking the user for an
   isolated stem. The user supplying the reference for their own analysis is
   a different question from us shipping weights; counsel should confirm
   both.
2. *Separation leaks reverb into the stem and removes some of it*; estimates
   on separated stems are biased and must be presented as approximate.
3. *Decay.* Blind RT60 estimation from reverberant signals: R. Ratnam et al.,
   "Blind estimation of reverberation time", JASA 114(5), 2003 (maximum
   likelihood on free-decay segments); H. Löllmann, E. Yilmaz, M. Jeub,
   P. Vary, "An improved algorithm for blind reverberation time estimation",
   IWAENC 2010. Benchmark and methods for RT60 and DRR: J. Eaton,
   N. Gaubitch, A. Moore, P. Naylor, "Estimation of room acoustic parameters:
   the ACE Challenge", IEEE/ACM TASLP 24(10), 2016. These were developed on
   speech in rooms; on produced music with artificial reverb, gated or
   ducked tails and compression they are untested — **open, needs our own
   validation set** (render our own reverb at known settings over licensed
   dry stems, estimate, compare).
4. *Direct-to-reverberant ratio* and wet level: DRR estimators from the ACE
   Challenge; alternatively a dereverberation pass (WPE: T. Nakatani et al.,
   "Speech dereverberation based on variance-normalized delayed linear
   prediction", IEEE TASLP 18(7), 2010; `fgnt/nara_wpe`, MIT) and compare the
   loudness of the dereverberated stem with the original.
5. *Early-reflection density / size*: echo density profile (J. Abel and P. Huang, "A simple, robust measure of
   reverberation echo density", AES 121st Convention, 2006) on the
   estimated late part — low confidence on separated music stems; **open**.
6. *Delay time and feedback*: autocorrelation or cepstrum of the stem's
   envelope/spectrum shows peaks at the echo lag (B. Bogert, M. Healy,
   J. Tukey, "The quefrency alanysis of time series for echoes", 1963); the
   ratio of successive peak heights estimates feedback; snap the lag to the
   nearest note value at the reference's detected tempo and report the
   residual.
7. The output is a set of **our** reverb/delay parameters, then refined by
   rendering our plugin and minimising a feature distance to the reference
   stem (the Tier 2 loop of §2.2).

**How much is too much (analysis-time on rendered audio; meters live).**
- *Clarity of our own reverb.* We know our reverb's impulse response
  exactly at any setting, so C50, C80 and D50 (ISO 3382-1) are computed from
  the IR directly — no estimation. Report them as "how much the tail smears
  the source" per proposed setting.
- *Wet vs dry loudness.* BS.1770 short-term loudness of the wet return vs
  the dry source on the same section; the suggestion carries a target window
  per style (our authored table) and flags wet within a few LU of dry on a
  lead vocal.
- *Masking against the lead.* Excitation-pattern masking of the wet returns
  against the lead vocal and against the rest of the mix (Moore, Glasberg,
  Baer 1997; Hafezi and Reiss 2015). Suggest wet-path EQ (HPF/LPF), ducking
  the return from the dry (side-chain), or a shorter decay where masking of
  the lead exceeds a threshold.
- *Reference comparison.* The same three numbers computed on the user's
  reference stem (where separation allows) give the "artist's amount".
- All thresholds are ours to calibrate by listening test (§2.4); none is a
  published standard.

**Applying suggestions.** Each suggestion is a resolved operation on the
session (ADR-0009, `brain/specs/timeline-model.md`): create an FX return
bus, insert our reverb/delay with parameters, create sends at given levels
from the chosen tracks, insert wet-path EQ or ducking. One suggestion is one
undo step, shown with its reasons (tempo, note value, clarity, masking
figures) before it is applied. Nothing changes the audio until the user
accepts it.

**Real time vs analysis.** Tempo detection, separation, blind estimation,
masking and clarity analysis, and parameter fitting: analysis-time, off the
audio thread (minutes for separation of a song on CPU — not measured here).
In real time: the reverb and delay themselves, the tempo-synced delay
following the tempo map, side-chain ducking, and live wet/dry loudness
meters.

### 2.6 On-device inference

- **ONNX Runtime** (MIT, `microsoft/onnxruntime`): CPU execution provider on
  Linux and Windows x64/arm64. It promises no real-time behaviour — its
  session run uses internal allocators and thread pools — so it does not go on
  the audio thread (CLAUDE.md: no allocation, no locks there). Use it in
  analysis passes, in the engine's non-RT workers or a separate process.
- **RTNeural** (BSD-3-Clause) is designed for per-sample inference without
  allocation (arXiv:2106.03037) and is what NAM-class and LSTM amp models
  should use inside plugins.
- **Size vs latency at q128 (estimates, not measured).** A per-sample LSTM
  with 32 units ≈ 4·(32·33+32·1)·2 ≈ 9 k MACs/sample ≈ 1.1 M MACs/block per
  channel — class B–C. A small WaveNet amp model is of the same order. A
  CREPE-full–class pitch net or any mixing/mastering network is orders of
  magnitude above that and is not block-causal (64 ms+ windows, whole-song
  context): offline/analysis only.
- **Verdict.** Per-block real time: amp/cab neural models, match-EQ filters,
  the real-time pitch tracker (DSP, not a net), meters. Analysis-time only:
  mastering analysis, timbre-match optimisation, key detection,
  CREPE-class note maps, the mixing assistant, source separation.

---

## 3. Training data licensing

Only own recordings or data whose licence permits use in a GPL-3 product
(commercial redistribution allowed). Attribution must ship with the product
(CC BY requires it).

| Dataset | Licence (checked 2026-10-13) | Usable |
|---|---|---|
| SignalTrain LA-2A (Zenodo 3824876) | CC BY 4.0 | yes, with attribution |
| GuitarSet (Zenodo 3371780) | CC BY 4.0 | yes |
| VocalSet (Zenodo 1442513) | CC BY 4.0 | yes |
| NSynth (magenta.tensorflow.org/datasets/nsynth) | CC BY 4.0 | yes |
| MedleyDB | CC BY-NC-SA (non-commercial research) | **no** |
| MUSDB18 / -HQ (Zenodo 1117372) | educational use only | **no** |
| madmom model files | CC BY-NC-SA 4.0 | **no** |
| Essentia models | non-commercial (BY-NC-SA vs BY-NC-ND, sources disagree) | **no** |

**Pretrained weights inherit their data's problem.** CREPE's paper trains on
RWC-synth and MDB-stem-synth, the latter resynthesised from MedleyDB stems
(read from the paper). The code is MIT, but shipping its released weights
in a commercial-redistributable GPL product is not clearly permitted —
retrain on CC BY / own data. The same check applies to every pretrained model
before it ships.

GPL-3 also asks for the "preferred form for modification": for a shipped
network that arguably includes training code and a data recipe. Open
question; the conservative answer is to publish both.

## 4. Open-source code we can learn from or reuse

Licences from GitHub metadata and, where marked, file headers (2026-10-13).

| Project | Licence | Compatible with GPL-3.0-or-later |
|---|---|---|
| x42 fil4.lv2, fat1.lv2 | GPL-2.0-or-later (headers) | yes |
| x42 dpl.lv2 | GPL-3.0 | yes |
| x42 meters.lv2 | GPL-2.0 (metadata; headers unchecked) | check |
| Ardour a-comp/a-eq/a-delay/a-reverb | GPL-2.0-or-later (header) | yes |
| zam-plugins (ZamTube, ZaMaximX2, ZamComp) | GPL-2.0-or-later (headers) | yes |
| LSP Plugins | LGPL-3.0 | yes |
| Calf Studio Gear | LGPL-2.1 | yes |
| Airwindows | MIT | yes |
| Dragonfly Reverb | GPL-3.0 | yes |
| CHOW Tape (AnalogTapeModel) | GPL-3.0 | yes |
| chowdsp_wdf | BSD-3-Clause | yes |
| chowdsp_utils | per-module licences | check each module |
| ACME.jl | MIT | yes |
| Guitarix | GPL (per-file) | check |
| Surge XT | GPL-3.0 | yes |
| libebur128 | MIT | yes |
| pyloudnorm | MIT | yes (test oracle) |
| Matchering | GPL-3.0 | yes |
| Rubber Band | GPL-2.0-or-later / commercial | yes |
| Signalsmith Stretch | MIT | yes |
| pYIN (c4dm) | GPL-2.0-or-later (header) | yes |
| aubio | GPL-3.0 | yes |
| CREPE, torchcrepe (code) | MIT | yes; weights see §3 |
| Neural Amp Modeler core, plugin, trainer | MIT | yes |
| neural-amp-modeler-lv2, Proteus, GuitarLSTM, AIDA-X, Automated-GuitarAmpModelling | GPL-3.0 | yes |
| RTNeural | BSD-3-Clause | yes |
| ONNX Runtime | MIT | yes |
| DDSP | Apache-2.0 | yes |
| micro-TCN | Apache-2.0 | yes |
| FxNorm-automix | MIT | yes |
| automix-toolkit | Apache-2.0 | yes |
| Demucs (code) | MIT | yes; weights see §2.2 |
| Open-Unmix (code) | MIT | yes; `umxl` weights CC BY-NC-SA 4.0: **no**; `umxhq` MUSDB18-HQ: **no** |
| Spleeter (code) | MIT | yes; weights' training data undocumented in README: check |
| nara_wpe | MIT | yes |
| librosa | ISC | yes |
| Essentia | AGPL-3.0 (code) | combinable with GPL-3 but adds network clause; models no |
| JUCE | AGPL-3.0 / commercial | same caveat; not needed |
| RAVE | CC BY-NC 4.0 | **no** |
| DeepAFx-ST | Adobe Research License, noncommercial | **no** |
| Diff-MST | CC BY-NC-SA 4.0 | **no** |
| madmom models | CC BY-NC-SA 4.0 | **no** (code BSD: yes) |

Reuse of GPL-2.0-or-later code is fine; GPL-2.0-only would not be, and none
was found in this list.

## 5. Legal notes (engineering reading, not legal advice — counsel to confirm)

**Safe in principle**
- Functional and sonic inspiration: a leveller that behaves like an optical
  one. Functionality is not protected expression (CJEU, *SAS Institute v World
  Programming*, C-406/10, 2012; the same position ADR-0004 takes).
- Modelling a circuit's behaviour from its topology and component values,
  measured on hardware we own or rent, and from published papers.
- Our own names, our own presets, our own IRs and captures.

**Not safe**
- Copying code, presets, IRs, captures, samples or artwork of any product
  (ADR-0004).
- Training on, or black-box fitting, a commercial plugin's output (the
  coordinator's rule; it clones the plugin's algorithm).
- Third-party trademarks in product, plugin, preset or UI names: Teletronix,
  LA-2A, UREI, 1176, Universal Audio, UAD, Avid, Pro Tools, Auto-Tune,
  console makers' names. Even "-style" naming suggests endorsement (US
  Lanham Act §43(a), 15 U.S.C. §1125(a), false endorsement; ADR-0004 bars it).
  Documentation may refer to the original hardware descriptively in research
  notes like this one; product copy should not.
- Artist names in presets ("<artist> lead tone"): trademark where registered,
  and right of publicity (e.g. California Civil Code §3344). Use reference
  matching from user-supplied audio instead (§1.11).
- Redistributing scanned schematics or manual text (copyrighted documents);
  transcribe the circuit facts into our own netlists.
- Shipping models trained on NC or educational-only data (§3).
- Patents: the 1998 auto-tune patent appears expired; other pitch-correction
  and polyphonic-editing patents may be live. A patent search is needed before
  the tuner ships — **open**.

## 6. What must be measured here (none of it is yet)

On the target machine with the UMC1820 and the RT kernel, quantum 128, 48 kHz:

1. Worst-case per-block time of each plugin class (A–D) as implemented, to
   replace every estimate in this note.
2. How many class-C instances (tape at 4x, FET at 4x, NAM model) fit beside 18
   recording tracks without xruns.
3. RTNeural vs ONNX Runtime CPU latency for an LSTM-32 amp model, per block.
4. Real-time tuner latency and correction quality with YIN vs pYIN on VocalSet.
5. Our LA-2A-style model's error against SignalTrain at each setting.
6. Blind RT60/DRR and echo-lag estimators on separated music stems with
   artificial reverb of known settings (our own renders over CC BY stems).
7. Hardware captures: no FET-limiter, tape machine or guitar amp has been
   captured; which units the project can own or rent is undecided.

## Decisions this forces (ADRs before code)

- Where first-party DSP lives (e.g. a `plugins/native/` module in the ADR-0007
  DAG) and whether our own plugins run in-process (trusted, no IPC cost) or
  out-of-process like third-party ones.
- The oversampling and antialiasing policy (default factors, ADAA where
  applicable, user quality switch) and how it is reported to PDC.
- Whether black-box models of **hardware** (SignalTrain, our own captures)
  are allowed for shipped models, or only for validation.
- The inference runtime split: RTNeural on the audio thread, ONNX Runtime in
  analysis workers or a separate process.
- The data and weights policy of §3, including publication of training code.
