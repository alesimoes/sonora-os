# ADR-0165 — Stem separation contract (logic.stem_split)

Status: accepted.

## Context

Logic-style stem splitting turns a region into vocal/drum/bass/other or
harmonic/percussive tracks. It is heavy offline work; no free ML model has
been licence-checked, and the image carries no ONNX runtime.

## Decision

`common/stems` defines `StemSeparator` (`name`, `supports`, `separate`), a
`ProgressSink` whose `report()` returning false cancels, `FakeSeparator`
(each stem = input / 4, sum equals input) and `default_separators()`.
It runs on a worker/disk thread only, never the audio thread.

1. v1 ships a classical baseline: HPSS median-filter harmonic/percussive and a
   mid/side vocal-centre estimate. It is labelled honestly as not ML quality.
2. An ML backend (e.g. a free-licensed Demucs-class model) plugs into the same
   interface later; content hosted per ADR-0004; the licence must be verified
   before any weights ship.
3. Output stems are rendered to new files and imported as tracks; the original
   is untouched.

Build hook: `common/groove/CMakeLists.txt` carries one guarded
`add_subdirectory(../stems)`, and tests are labelled `stems` inside
`common/stems/CMakeLists.txt`, because time.tempo_map and time.elastic own the
root `CMakeLists.txt` and `tools/module-deps.allow`. The hook and the allow
line move to the root later.

## Rationale

One interface lets the baseline and an ML model swap without touching
callers; cancellation through the sink keeps long jobs stoppable.

## Consequences

`default_separators()` is empty until the baseline lands. The module is not
yet in the dependency allow-list.

## Alternatives rejected

- Bundling an ML model now: licence and weight size unverified, ONNX runtime
  dependency on the image.
- A blocking in-process call on the engine: stalls the audio thread.

## Unverified

- Licence of any candidate ML model and its weights.
- Model size on disk.
- CPU time per minute of audio, baseline and ML.

Handed to the researcher.
