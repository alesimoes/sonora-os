# ADR-0181: The tuner's real-voice bars and the YIN / pYIN decision

Status: accepted. Amends ADR-0077 (points 4 and 5); supersedes nothing.

## Context

ADR-0077 built `native_tuner_eval` and left two things for the first measured
run: the real-voice bars (point 4) and the YIN-or-pYIN decision (point 5). The
run is done (`brain/research/native-tuner-eval.md`, results table): vocadito
(Zenodo 5578259, 40 clips) and a 101-clip VocalSet subset (scales/straight,
Zenodo 1442513, `VocalSet11.zip`), on an i7-11800H, GCC 14.2 Release, commit
`a5bd87ac`. vocadito's CSV layout (`time_s,f0_hz`, 256/44100 s hop, 0 Hz =
unvoiced) matched the loader, so it did not change. Measured, YIN: RPA 0.922 /
0.917 / 0.716 for the low / medium / high detector ranges, voicing recall about
0.94 and false alarm 0.11 to 0.13 (low, medium); in-scale fraction 0.806
(vocadito) and 0.889 (VocalSet).

## Decision

1. The real-voice bars are in `plugins/native/tuner/quality.json` under
   `real_voice`: vocadito low RPA >= 0.88, medium >= 0.87, high >= 0.66 (plus
   RCA, voicing recall and false-alarm bars per range), vocadito in-scale >=
   0.75, VocalSet in-scale >= 0.84. Each sits 4 to 6 points inside the measured
   figure. They are checked by an operator run of `native_tuner_eval`, since
   the datasets are never in CI; the synthetic bars remain the ctest gate.
2. YIN stays. ADR-0077 point 5 adopts pYIN only if our YIN is more than 3
   points below pYIN's published vocadito RPA and pYIN fits class C. No
   published pYIN figure exists on vocadito, so the first condition cannot be
   met; pYIN is not adopted and is not recorded as "rejected on numbers",
   because there is one number, not two.
3. Processor, parameters, latency and the audio path are unchanged.

## Rationale

Headroom follows how the synthetic bars were set: other compilers and the
float order change the last digits, not whole points. The "high" range scores
far lower on this set; its bar is a regression floor and no quality claim.
The vocadito paper's baselines are Vocano (notes) and CREPE (f0); the f0
labels were seeded by pYIN and then hand corrected, so a pYIN score on them
would be biased in its favour and is a weak basis for replacing YIN.

## Consequences

- A regression in the real-voice figures is visible to anyone with the data;
  CI cannot see it.
- Re-opening pYIN needs its own measurement (an independent implementation run
  on vocadito, with the labels' seeding noted) and a new ADR.
- The in-scale bars are against C major on audio in other keys: they measure
  snap accuracy, not musical correctness.

## Alternatives rejected

- **Implement pYIN to get its number.** A GPL reference cannot be copied, and a
  clean implementation for a comparison nobody asked to ship is cost without a
  decision attached; the 3-point rule needs a published figure.
- **Use CREPE's 0.98 as the target.** A neural model, outside the classical
  design of ADR-0011 for this processor, and a different class of CPU.
- **Tighter bars (at the measured value).** Fail on another compiler for no
  audible reason.
- **Bars from the whole VocalSet.** Only 101 straight-tone scale clips were
  run; the bar says so rather than claiming the set.

## Unverified

- pYIN's RPA on vocadito: no published figure found (vocadito paper, Tables 3
  and 4 only).
- The bars under MSVC and Clang: only GCC 14.2 was run.
- VocalSet: the `VocalSet11.zip` archive and a subset were used, not v1.2's
  full set; latency and CPU on the busiest clip were not measured (the tool
  does not report them).
- The tool's `version` field still prints "as downloaded; not recorded by the
  tool"; versions are recorded in the research note.
