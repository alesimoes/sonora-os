# Amp and cabinet capture rig: procedure, measurements, and what the owner provides

Status: procedure and checklist, 2026-11-07. Written for ADR-0011 N3 (amp and
cabinet) and the capture contract of ADR-0069 (capture cards). **No capture
or IR is produced by this feature until the owner names the hardware** (§2).
Until then this note is the plan the first session follows, and every number
in it that is a guess is listed in §10 with the measurement that settles it.

The rules this procedure serves (ADR-0011 §4.6, §5): we capture only hardware
we own or rent; we never capture a commercial amp *plugin* (that is black-box
cloning); we never use a third-party capture or IR without a written licence
that allows redistribution; every shipped capture and IR has a card before it
enters a manifest.

## 1. Chain for a NAM-class capture (reamp)

A neural amp model (NAM-class: a WaveNet or LSTM trained to map a dry input to
the recorded output of a real amp) is trained on a pair of files: the input
we sent and the output the amp produced. The chain that makes the pair:

```
interface out (line, balanced)
  -> reamp box (passive, transformer-isolated; sets the level)
  -> amp input (guitar level)
  -> amp, at the power amp's operating point (see §1.2)
  -> load box or speaker-level attenuator, or a cab / mic chain
  -> interface in (line, balanced), recorded dry-to-wet, same clock
```

Recording dry and wet on the same interface, from the same clock, makes the
pair sample-aligned (the latency is measured, §5, not guessed).

### 1.1 Levels

- Send side: the reamp box is set so that the input file's peak sits at
  **-18 dBFS** on the interface output. Hot enough to drive the amp into the
  range the player uses, not so hot that the input clips inside the amp.
- Return side: the wet recording peaks no higher than **-6 dBFS**, with
  -1 dBFS as the hard ceiling of the interface input. Headroom above the
  recorded peak is kept for the model's overshoot.
- Loudness of the pair is matched before training (§1.3); the level choice
  does not change the amp's operating point beyond the gain the player sets.

### 1.2 Amp operating point

- Record each channel and each gain/tone setting that will ship as its own
  pair. Settings are written on the card (`settings`), as numbers or names.
- Power amp: run at the same power-section operating point the card records
  (volume, presence, master). Use a load box or attenuator where the amp needs
  a load; a speaker-level attenuator is allowed only where the owner's
  equipment rules permit it.
- Warm up the amp for the same time before every session, and log it in the
  session note. Tube drift changes the target.

### 1.3 Input signal and training

- The trainer's **standard input signal** (the sweep-and-noise file that
  NAM-class trainers ship as their reference input) is used only if its licence
  is recorded and allows our use. If the licence is unclear, we generate our
  own input: a log sweep plus a seeded, band-limited noise burst, with the
  generator's code and seed committed (no hidden file). **Unclear means
  excluded** (ADR-0011 §4.6). The licence check is the researcher's (§10, item 5).
- The input file is ours or freely licensed, and its hash is recorded in the
  card's `rig` text or the session note.
- Training is done on the owner's laptop or the training machine named in the
  card; the commit of the trainer is recorded as `train_commit`.
- Train on the first part of the recording; the held-out part (§6) is never
  seen by the trainer.

## 2. What the owner must provide (checklist to confirm)

Nothing here is ordered or booked by this feature. The owner confirms each line.

| # | Item | Owned or rented | Confirmed |
|---|---|---|---|
| 1 | Amp unit(s): type class (for example, a two-channel EL34 head, 50 W), serial number, channels to capture | [ ] | [ ] |
| 2 | Cabinet unit(s): type (for example, a 4x12 closed-back), serial, speakers | [ ] | [ ] |
| 3 | Microphone(s) for cab IRs, and their type (dynamic, cardioid, etc.) | [ ] | [ ] |
| 4 | Audio interface: the owner's **UMC1820-class** 18-channel USB interface (the target hardware), set to 48 kHz, 24-bit | [ ] | [ ] |
| 5 | DI box (guitar to line, for the dry reference and the player's reference) | [ ] | [ ] |
| 6 | Reamp box, passive, transformer-isolated, with level control | [ ] | [ ] |
| 7 | Load box or speaker-level attenuator for the power amp | [ ] | [ ] |
| 8 | Cables, stands, and a quiet room (noise floor measured, §5) | [ ] | [ ] |
| 9 | Written rental terms for every rented unit (§8), signed before the session | [ ] | [ ] |

A unit we do not own or rent is not captured. Renting is a legal act as well
as a practical one: the lessor's written consent comes before the session.

## 3. Sample rate and bit depth

- **Sample rate: 48 kHz** for the reamp pair and the IR, matching the card's
  `capture.sample_rate` (and the target interface setting). Whether 44.1 kHz
  would serve as well is an open item (§10, item 9).
- **Bit depth: 24-bit integer WAV** for both the dry and the wet recording, and
  for any IR. The trainer reads floating-point or 24-bit input; the 24-bit
  files are the masters and are not re-quantised before training.
- Never record at a rate the interface resamples internally: set the rate on
  the interface and in the DAW session, and check the session's rate on the
  first file.
- Clock: one interface, one clock. No second clock source during a session.

## 4. Noise floor and the capture chain

- Before the first pair, measure the chain's noise floor with the amp off and
  the input at zero: record 30 s, measure the RMS in dBFS. The chain must sit
  at least **40 dB** below the wet recording's peak (figure to confirm, §10
  item 6).
- Hum is recorded in the same session: note the mains frequency and whether
  the rig is earthed as the owner's own practice.

## 5. Latency and alignment

- Measure the dry-to-wet latency once per setup by a click or a sweep through
  the chain, and record it in the session note. Align the pair to the measured
  latency before training. An unmeasured latency is a bug, not a default.
- A pair whose alignment is off by more than one sample at 48 kHz is rejected
  and re-recorded.

## 6. Held-out error-to-signal ratio (ESR)

ESR is the figure the card records as `esr_held_out`, and the bar a shipped
capture must clear is set per architecture by the class-E measurement
(ADR-0011 Unverified 9). This note fixes how it is measured, so the number is
comparable across captures.

- **Definition.** ESR = Σ (y − ŷ)² / Σ y², summed over the held-out segment,
  where y is the recorded wet signal and ŷ the model's output for the same
  dry input. Both are at the same sample rate, time-aligned by the measured
  latency (§5), and at the recording's own level (no gain applied to either).
- **Held-out segment.** A stretch of the recording that the trainer never sees.
  The default is the **final 20 %** of the input file, cut at a phrase boundary
  of the input, with at least 10 s. A second held-out take with a different
  playing style is recommended but not required for v1 (follow-up).
- **Recording.** The ESR, the held-out segment's start and length in samples,
  the architecture, and the `train_commit` go on the card. A capture whose
  card has no ESR is refused by policy (ADR-0069 §3).
- **The bar.** Per architecture, the bar is the ESR that the class-E
  measurement says the architecture can reach on our own captures, and the
  card must clear it to ship. The bar is **not set in this note**: it is an
  output of ADR-0011 Unverified 9 and is recorded as a table in the
  measurement note before the first capture is made. Until that table exists,
  no capture ships. (Follow-up: `validate_card` does not yet compare ESR to
  the bar; it only requires that ESR is present and not negative.)

## 7. Cabinet IR: log-sweep deconvolution

- **Method.** Exponential (log) sweep, deconvolved against its inverse filter,
  after Farina, "Simultaneous measurement of impulse response and distortion
  with a swept-sine technique" (AES 108th Convention, 2000). The sweep runs
  from 20 Hz to 20 kHz, about 10 s, with 1 s of silence before and after, at
  48 kHz / 24-bit. Harmonic distortion products land before the main
  impulse and are discarded by the deconvolution, which is why the exponential
  sweep is chosen over a linear one.
- **Source.** The sweep is generated from the code committed with the card's
  `rig` note. It is our own signal, so it carries no licence question.
- **Level.** The sweep is played through the power amp and cabinet at a level
  that does not drive the speaker into compression. Record the level in dBFS
  on the interface input, and a speaker-thermal note if the cab was driven
  hard.
- **Trim.** The IR is trimmed to the length where the energy decay (Schroeder
  backward integral) reaches -60 dB, then faded out over the last 5 ms. The
  trimmed length must fit the class-E cabinet budget of **200 ms** by
  partitioned convolution (ADR-0011 Unverified 9). If a longer tail is
  audible, the trim is a decision recorded on the card, not a silent cut.
- **Microphone position notes.** Each IR's `position` records: distance from
  the grille (cm), the axis (on-axis or off-axis, with the angle), the place on
  the cone (centre, cap edge, between), the mic's type, and the room's
  dimensions. Two placements of the same cab are two cards, never one card
  with a "variant" field. The `microphone` field records the mic type
  (`dynamic cardioid`), not a brand; the real unit goes into the `rig` text,
  which is provenance only.
- **Repeatability.** Re-set the mic at the same mark after moving it away and
  measure the two IRs against each other (§10, item 7).

## 8. Rental-terms checklist

Before any rented unit is captured (ADR-0011 §4.6: "the rental terms do not
forbid recording it"). The owner signs; the card records only the result.

- [ ] Lessor (name kept with the paper, not on the card) and rental period.
- [ ] Written permission to **record** the unit's sound (the test that the card's
      `rental_terms_allow_recording: true` attests to).
- [ ] Written permission to **distribute a derived file** (a capture or an IR
      shipped in the product). Recording permission alone is not enough: the
      shipped file is a derivative, so this clause is a separate check.
- [ ] No clause forbidding modification, including connection to a reamp box,
      a load box or a DI box.
- [ ] Serial number of the unit recorded on the card matches the unit on the
      rental paper.
- [ ] Return condition and insurance noted; no damage is caused by the rig
      (a load box is checked against the amp's manual, not the lessor's word).
- [ ] If any line is unclear, the unit is not captured. Unclear means excluded.

## 9. Deny-list seed (for the name checker)

The checker (`plugins/native/ampcab/tools/ampcab_name_deny.txt`, read by
`parse_name_deny`) refuses a shipped `id`, `name` or `file.name` that contains
a deny term once lower-case alphanumerics are compared (ADR-0069 §3). The
**terms themselves are not written in this note**, because ADR-0011 §5 bars
brand, artist and model names from docs and code. The researcher compiles the
list into the deny file, which is the checker's input and is not product text.

The seed **categories** the list must cover:

- amp manufacturers and their sub-brands;
- cabinet and speaker manufacturers;
- microphone manufacturers;
- commercial amp and effect plugin names, of any maker;
- guitarists, bassists, band names, record titles and artist names;
- model numbers of the above, including near-variants (a letter-digit code
  one character off a real model), since the checker normalises case and
  punctuation and so matches "ZX-900", "zx 900" and "zx900" alike.

Example of the form (fictitious, the one the test suite already uses):
`ZX-900`. A term is added when a name is refused by a human, and the reason is
recorded in the deny file's comment.

## 10. Unverified items, each with the measurement that settles it

For the researcher. Each item stays open until its measurement is in a note
under `brain/research/` and the card or this note is updated.

1. **Capture ESR per architecture on real hardware.** Settled by training each
   architecture (the WaveNet sizes from standard down to the smallest, and the
   LSTM) on one owned pair, and measuring §6's ESR on the held-out segment.
   The table of ESR by architecture is the bar. Not settled until run on the
   owner's hardware.
2. **Class-E cost of the chosen architecture** (ADR-0011 Unverified 9): per-block
   cost, p99.9 and max at quantum 128, on the owner's laptop, with a 200 ms
   cabinet IR. Settled by the dsp-engineer's benchmark, before the first
   capture session.
3. **Loudness normalisation level.** Settled by measuring the integrated
   loudness (BS.1770) of the recorded wet file and of the model's output on the
   held-out segment: the difference must stay within the stated tolerance
   (proposed 0.5 LU) at the -18 dBFS send level of §1.1. If not, the send level
   is moved, not the target.
4. **IR trim length against the 200 ms budget.** Settled by the Schroeder decay
   of each measured IR (the length where the decay reaches -60 dB) and the
   partitioned-convolution cost at that length, against Unverified 9's class E.
   If the tail to -60 dB is longer than 200 ms, the trim decision is recorded
   on the card with its audible effect.
5. **Licence of the trainer's standard input signal.** Settled by reading the
   licence text the trainer ships with the file. If it is unclear, the
   generated log-sweep-plus-noise input of §1.3 is used instead. Recorded on
   the card's `rig` text.
6. **Noise floor required.** The 40 dB margin of §4 is a proposal. Settled by
   measuring the amp-off floor on the owner's chain and the resulting ESR
   contribution of the noise, on the held-out segment.
7. **Microphone position repeatability.** The claim that two placements at the
   same mark repeat is settled by measuring the spectral difference of two IRs
   re-set at one mark, in dB, per octave band. If the difference exceeds the
   tolerance the position notes must add a placement jig.
8. **Sample rate: 48 kHz or 44.1 kHz.** Settled by training the same pair at
   both rates and comparing ESR and class-E cost. 48 kHz is the default until
   then.
9. **Rental terms permit derived files** (§8). Not measurable: settled by the
   signed written permission. The researcher checks the paper, not the sound.
10. **Reamp level for the target amp.** The -18 dBFS send and -6 dBFS return of
    §1.1 are proposals. Settled by sweeping the send level in 3 dB steps on
    the owner's amp and choosing the level at which the model's ESR is lowest
    without clipping the wet recording.

## 11. Session procedure, in order

1. Confirm §2 (owned or rented, serials, signed rental terms if any).
2. Measure the noise floor (§4), the latency (§5) and the interface rate.
3. Record the input (§1.3) dry and wet, per channel and setting, at §1.1 levels.
4. Record the IR sweeps (§7), with the mic position written down the moment
   it is set.
5. Split each recording: training part, held-out part (§6).
6. Train; measure ESR on the held-out part; record the held-out bounds.
7. Write the card (`brain/models/TEMPLATE-capture-card.md`): fill every
   placeholder, compute `sha256` and `size`, run `parse_card` and
   `validate_card`.
8. Put the file in the capture manifest only after the build check passes.

## Sources

- Farina, A. (2000). Simultaneous measurement of impulse response and
  distortion with a swept-sine technique. AES 108th Convention, preprint 5093.
- Schroeder, M. R. (1965). New method of measuring reverberation time. J.
  Acoust. Soc. Am. 37(3), 409–412 (backward integration of the impulse
  response, §7).
- ADR-0011 §4.6 and §5; ADR-0011 Unverified 9; ADR-0069 §§1–3, 5.
