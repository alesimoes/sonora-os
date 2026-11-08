# ADR-0077: The tuner's evaluation on real voices

Status: Proposed (architect review pending).
Builds on: ADR-0004 (no black-box cloning), ADR-0011 §3.2 (validation is measurement), §3.5 (CPU classes), §4.6 (training and evaluation data), the tuner's `quality.json` (PR #383).

## Summary

The tuner's DSP (YIN detector, PSOLA shifter, key/scale chooser) is on main and gated by synthetic bars in `plugins/native/tuner/quality.json`. This ADR adds an evaluation on real singing: vocadito (frame f0 annotations) and VocalSet (no f0 labels), both CC BY 4.0. Datasets live on the operator's disk and are never committed or fetched; metrics follow mir_eval's melody definitions; the report is JSON of numbers only; real-voice bars are set from the first measured run; YIN stays unless the numbers say otherwise. The contract is `plugins/native/tuner/eval/include/sonora/native/tuner_eval.h`; nothing in `Processor`, its parameters or the audio path changes.

## Context

Synthetic tones (harmonic stacks, noise at 20 dB SNR, missing fundamentals) prove the detector and shifter behave, but a singing voice adds vibrato, breath, onsets, fry and formant motion the synthetic set does not. The BACKLOG line for `native.tuner` asks for the evaluation on real voices. The two usable datasets are cleared in `brain/research/open-data-and-dsp-sources.md`: vocadito (Zenodo 5578259, 40 solo excerpts with frame f0 from annotators) and VocalSet (Zenodo 1442513, sung techniques, no f0). Both are CC BY 4.0: usable with attribution, but large, and redistributing them in the repository or the image is neither needed nor wanted.

## Decision

1. **Data stays outside the build.** No dataset file is committed, and no build, test or CI step downloads one. The evaluation tool takes `--data DIR`, a local directory the operator prepared. When the directory is absent or empty, the real-data run is skipped with a message naming the dataset and where to get it; a skip is reported as `skipped`, never as a pass. The contract test uses an in-memory `FakeAnnotationSource`.
2. **Metrics, as mir_eval defines them** (`mir_eval.melody`, Salamon et al. 2014), on a common time grid (resampling the reference onto the estimate's hop is the `AnnotationSource` caller's step, as `mir_eval.melody.resample_melody_series` does):
   - *raw pitch accuracy* (RPA): over reference-voiced frames, the fraction whose estimated pitch is within 50 cents of the reference, whatever the estimate's voicing flag, as long as it reports a pitch;
   - *raw chroma accuracy* (RCA): the same with the error folded to the nearest octave, so an octave error counts for chroma and not for pitch;
   - *voicing recall*: reference-voiced frames the estimate calls voiced;
   - *voicing false alarm*: reference-unvoiced frames the estimate calls voiced.
   For VocalSet, which has no f0 labels, the processor-level figures that need no ground truth: the fraction of voiced output frames within X cents of a note of the chosen scale, measured by an estimator independent of the detector (the Hann-windowed DFT peak by golden-section search `quality.json` already uses), and the wander of the reported delay within one period. X is set with the bars (point 4).
3. **The report is numbers.** `render_json` writes JSON with the metric values, the frame count, the dataset's name, licence and version. It never contains audio, samples, file excerpts or anything derived that reconstructs the signal. Reports may be attached to PRs and stored in `brain/research/`.
4. **Bars come from a measured run.** The first run on each dataset sets the real-voice bars with headroom for the other compilers, the way `quality.json`'s bars were set, and records them, the machine, the commit and the dataset version in `brain/research/native-tuner-eval.md`. Until that run exists the real-voice figures are unverified and the synthetic bars in `quality.json` remain the plugin's gate.
5. **YIN is kept.** pYIN (Mauch and Dixon, ICASSP 2014) is adopted only if both hold: the measured RPA of our YIN on vocadito is more than 3 points below pYIN's published figure on the same data, and a pYIN implementation fits CPU class C (ADR-0011 §3.5) on the tuner's busiest case. Otherwise pYIN is recorded as rejected in the research note with the two numbers. pYIN's reference code is GPL-2.0-or-later; an adoption is a clean implementation from the paper.

The contract header declares `PitchFrame{time_s, hz, voiced}`, `EvalReport{rpa, rca, voicing_recall, voicing_false_alarm, in_scale_fraction, frames}`, the interface `AnnotationSource{load(path) -> std::vector<PitchFrame>}`, `score_detector(reference, estimate, tol_cents) -> EvalReport` and `render_json(EvalReport, dataset, licence, version)`. `score_detector` is pure and inline; `in_scale_fraction` is NaN (JSON `null`) when the run did not measure it. The contract test is `native_tuner_eval_contract` (label `native`). The evaluation tool, the vocadito CSV reader and the VocalSet run are the next task, off the audio path.

## Rationale

Ground-truth f0 is the only way to say how often the detector is right on a voice; mir_eval's definitions are what the literature reports, so our figures compare directly with published ones, which point 5 needs. VocalSet adds the techniques (vibrato, straight tone, breathy, fry) without labels, so it is scored on what the operator hears: does the corrected output sit on the scale. Keeping data off the repository keeps the image and the clone small and avoids redistributing even permissively licensed audio. Numbers-only reports keep the PR history free of dataset content. Bars from measurement, not guesses, is how every other native processor was gated.

## Consequences

- A developer with the datasets can produce the real-voice figures; CI without them skips with a visible message and the synthetic gate is unchanged.
- `brain/research/native-tuner-eval.md` becomes the record of the bars and of the pYIN decision.
- The tuner's `Processor`, its parameters, latency and CPU class are untouched by this ADR; a pYIN adoption would be a new ADR amendment with its own measurements.

## Alternatives rejected

- **Commit the audio (or a subset) to the repository.** Bloats every clone and the image, makes the repository a redistributor of third-party audio, and a subset small enough to commit is too small to measure on.
- **Auto-download in the build or CI.** Makes the build depend on Zenodo's availability, hides network access in a build step, and turns a missing dataset into a flaky test instead of an explicit skip.
- **Adopt pYIN now.** No measurement says YIN falls short on voices; pYIN's HMM costs CPU and latency, and its reference code is GPL. It is decided by point 5's numbers.
- **Score against our own detector's output on VocalSet.** Circular: the in-scale figure uses an estimator independent of the detector.
- **Train or fit to a commercial tuner's output.** Black-box cloning, refused (ADR-0004, ADR-0011 §5).

## Unverified

- pYIN's published RPA on vocadito: the vocadito paper's baseline table is to be checked; if it gives none, pYIN's figure is a run of the reference implementation recorded in the research note, outside this repository's code.
- vocadito's annotation hop and file layout (CSV per excerpt) as of the current Zenodo version.
- The real-voice bars themselves (point 4).
