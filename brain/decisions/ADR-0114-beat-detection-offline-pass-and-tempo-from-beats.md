# ADR-0114 - Beat detection: an offline sonora-analyze pass, applied as a tempo edit

- Status: Accepted
- Date: 2026-11-08
- Item: `time.beat_detection`
- Relates to: ADR-0011 (§3.4 inference only in sonora-analyze), ADR-0046
  (exit codes, report shape), ADR-0050 (tempo as session settings, Undo
  Tempo), ADR-0066 (§1 classical first, §5 subcommand fragments)

## Summary

Beat detection is a pass of `sonora-analyze`:
`sonora-analyze beat <file.wav> [--out report.json]`. It runs offline, never
on the audio thread, and is not an engine command; the protocol, the session
format and `contracts/` are unchanged. The result is onset and beat times in
source frames, a global bpm, a confidence from 0 to 1 and a tempo-stability
flag. A failed detection is an error and a low-confidence one is reported as
such, never guessed; the interface refuses to apply a result below 0.3. The
interface turns a result into a tempo map with the pure function
`tempo_from_beats` and applies it through ADR-0050's tempo operations as a
`SetSettings`, so it is undone with Undo Tempo. The `tempo.edit` text grammar
gains `detect`; there is no new `CommandSpec`.

## Context

Operators who record without a click want the session's bars to follow the
take: find the beats, set the tempo, and edit on the grid. Detecting beats
reads a whole file and takes seconds, and it allocates; it cannot go near
`Session::process()`. `sonora-analyze` already hosts offline passes (key,
timbre, tone, vocal balance) under ADR-0066's contract, and ADR-0050 already
defines how a tempo map is edited and undone without a protocol change.

## Decision

1. **The pass.** `sonora-analyze beat <file.wav> [--out report.json]` reads
   the file through `AudioSource` in blocks and writes one JSON report to
   stdout or `--out`. Exit codes as ADR-0046: 0 report written, 1 the pass
   failed (reason on stderr, no report), 2 usage, 130 cancelled. The build
   follows ADR-0066 §5: `app/cmd_beat.cpp` replaces `app/cmd_beat_stub.cpp`
   by existing; the slices add sources in `cmake/beat.cmake` and
   `cmake/beat_tempo.cmake`, which `plugins/analyze/CMakeLists.txt` includes
   when they exist.
2. **The interface** (`plugins/analyze/include/sonora/analyze/beat_detector.h`):
   `BeatDetector::detect(AudioSource&, const RunOptions&, BeatResult&, error)`,
   like `KeyDetector`. `BeatResult` = `sample_rate`, `frames`, `onsets[]` and
   `beats[]` (source frames, strictly increasing, inside the file), `bpm` in
   [30, 300], `confidence` in [0, 1], `stable`, `method`. `beat_fakes.h`
   holds `FakeBeatDetector` (a scripted result or error) and
   `fake_beat_result`.
3. **The report** is the contract, so its writer and reader are inline in
   the header: `{"pass": "beat", "schema": 1, "method": "spectral_flux",
   "sample_rate", "frames", "bpm", "confidence", "stable", "onsets": [...],
   "beats": [...]}`. Positions are integers so they round-trip exactly. The
   reader is strict: another `schema` number, an unknown key, another pass or
   an invalid result is refused by name. Changing the keys bumps
   `kBeatReportSchema` and amends this ADR.
4. **Never guessed.** A file too short, silent or without periodicity fails
   with exit 1. A detection that succeeds reports its confidence; the
   interface refuses to apply one below `kBeatMinConfidence` (0.3) and says
   why in a held notice.
5. **Applying.** `tempo_from_beats(beats, sample_rate, bpm, origin, meter,
   TempoFromBeatsMode, out, why)` (`beat_tempo.h`) builds the whole new map:
   `constant` puts one entry at the first beat at the global bpm; `follow`
   puts one per bar from the beat grid, for a result that is not `stable`.
   The map is checked by `tempo_map_problem` and applied as ADR-0050's
   `SetSettings`, so the undo is Undo Tempo and audio stays where it is in
   samples. The `tempo.edit` grammar gains `detect`; no `CommandSpec` and no
   edit to the command header.

## Rationale

- The pass shape, exit codes and fragment layout already exist and are
  tested for four passes; a fifth costs one stub and one table row.
- Integer frames make the report exact and match the model's `Frames`.
- Applying through the tempo operations reuses their checks and their undo
  stack, so nothing new is sent to the engine and nothing new needs saving.

## Consequences

- Detection runs where the interface starts it, on a file; the engine is not
  involved until the `SetSettings`.
- A future model-backed tracker is a new `BeatMethod` value, opt-in by name,
  never a silent fallback (ADR-0066 §1).
- No protocol bump, no session-format change, no file under `contracts/`.

## Amendment: the interface grammar (ui/beat)

`tempo.edit detect [clip|selection] [follow]` runs the pass on the selected
clip's first source through an injected `BeatRunner` (production:
`ProcessBeatRunner`, `sonora-analyze beat <file> --out <report>` read with
`parse_beat_report`; tests: `FakeBeatDetector`). The origin is the region's
`position - source_offset`, the meter the map's first entry's. Confidence
below `kBeatMinConfidence` is refused with the figures and the map
unchanged; `follow` is refused for a `stable` result. The map goes through
`EditController::submit_tempo`, so Undo Tempo reverts it. From `tempo.edit`
no runner is reachable yet, so the bare entry refuses plainly rather than
appearing to detect.

## Alternatives rejected

- **Detector inside the engine process.** It would put a seconds-long,
  allocating analysis next to the audio thread; ADR-0011 §3.4 keeps it in
  `sonora-analyze`.
- **A new wire message or a protocol bump.** The interface already starts
  `sonora-analyze` and already sends `SetSettings`; a message would add a
  version and a compatibility path for nothing.
- **A model-backed (ONNX) beat tracker as the default.** Classical first, as
  ADR-0066 §1: a spectral-flux onset envelope with an autocorrelation tempo
  and dynamic-programming beat tracking is built everywhere and needs no
  weights.
- **Region slicing at the beats ("Beat Detective" style) in this feature.**
  It edits clips, needs its own operations and undo, and is a separate item.
