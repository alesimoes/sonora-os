# ADR-0136 — Elastic audio: warp markers are an edit gesture, the stretch is an offline render into a new file

- Status: Accepted (recommended defaults, architect, 2026-11-09)
- Date: 2026-11-09
- Builds on: ADR-0052 §4 (destructive edits are copies; undo repoints), ADR-0051 (offline
  render, capability-gated, no protocol bump), ADR-0009 (sources append-only, playlists never
  overlap), ADR-0050 (tempo map), ADR-0008 (portability), ADR-0004 (workflow, not binaries)
- Progress item: `time.elastic`

## Summary

- §1 Warp markers (source frame -> target frame pairs) are held by the editor while the
  operator works. Nothing about them enters the session: **no session format, no protocol
  bump, no new Operation.** Applying renders a new file and repoints the region.
- §2 The engine command `audio.elastic` (capability `audio.elastic`) renders the region
  through a free stretcher into `Audio Files/<track>_elastic_NN.wav` (exclusive create,
  synced), then submits ONE Compound (AddSource, SetRegionSources, region trim/length).
  The take is never opened for writing; one undo step restores the original.
- §3 Stretcher: Rubber Band (GPL-2.0-or-later, compatible with our GPL-3.0-or-later),
  offline mode with a key-frame map. Hidden behind `common/stretch`'s `Stretcher` interface;
  only the engine adapter links it. Algorithm names map to its options: Polyphonic,
  Rhythmic (crisp transients), Monophonic (formant preserved), Varispeed (plain resample,
  pitch follows, no library).
- §4 Quantize Audio = onsets -> warp markers moving each onset to the grid (strength 0..100)
  computed by a pure planner in `common/stretch`, then §2.
- §5 A render that would grow past the next region is refused with a reason (playlists never
  overlap); no silent ripple.

## Decision

### 1. Why destructive-by-copy and not live warping
Live, non-destructive warping needs markers in the session (format bump, plan builder, a
stretcher on or beside the audio thread, resident pre-stretched chunks per ADR-0009). That is
a large RT-path change resting on unmeasured CPU. A copy reuses the proven ADR-0052 path, the
audio thread is untouched, and losing a take is impossible: the original file stays and undo
points back. Cost later: markers are forgotten after Apply (the operator re-warps the copy,
or undoes). A later ADR may add persisted markers with a format bump; this one does not
block it.

### 2. Engine command
`audio.elastic REGION ALGO MODE ...` (grammar in `api/elastic.h`): `ratio=R` (whole region),
`warp=SRC:DST,...` (markers, strictly increasing both sides), `quantize=GRID[:STRENGTH]`
(grid in beats, engine reads onsets and the tempo map). Limits: region at most
`kMaxElasticFrames` in memory, ratio within 0.25..4. Refusals are held notices (ADR-0052 §5).
The job runs on the control thread with progress events like ADR-0051; cancellable; a
partial file is deleted on cancel or failure.

### 3. Library gating
`find_package`/pkg-config for `rubberband`. Absent: the module compiles without the adapter
and the engine does NOT advertise `audio.elastic` (fails loudly: the menu says "not
available in this build"), instead of degrading to a poor fallback. Windows MSVC neutral
build compiles only the pure parts.

### 4. Onsets
The planner takes onsets as input (`std::vector<Frames>`). Source: the beat/onset detector
of `time.beat_detection` when merged, else the peak-envelope picker used by the groove UI;
chosen behind an injected interface so this ADR does not wait for either.

## Consequences
- No migration, old peers unaffected; peers without the capability see the command as
  NotImplemented.
- Offline render time scales with region length; measured later (Unverified 2).
- Stretch quality for drums vs vocals is subjective; the three algorithm presets are the
  knob, no tuning UI.

## Alternatives rejected
- **Live warp markers in the session**: format bump, RT-path risk, unmeasured CPU. Rejected
  for v1; revisit after measurement.
- **Own phase vocoder/WSOLA**: months of tuning for worse quality; Rubber Band is free and
  GPL-compatible.
- **Reusing `track.process_selection`**: it assumes length-preserving output.
- **SoundTouch (LGPL)**: weaker on polyphonic material; kept as the named swap behind the
  interface if Rubber Band is unavailable in the image.

## Unverified (for the researcher)
1. `librubberband-dev` present in the Debian trixie base, the CI build image and the ISO
   package list, and its version (R3 engine needs 3.0+). Measure: `apt-cache policy`.
2. Offline render speed of 3 min stereo 48 kHz, 4 algorithms, on the dev laptop and the
   target hardware (assumption: well above real time).
3. Key-frame map behaviour with a marker at a region's first and last frame (assumed exact
   length; the test must assert output length equals the target span).
