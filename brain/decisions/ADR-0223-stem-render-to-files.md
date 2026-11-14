# ADR-0223 — Stems rendered to files (logic.stem_split)

Status: accepted (recommended default).

## Summary

`StemRenderer` (`common/stems/include/sonora/stems/stem_render.h`) runs a
`StemSeparator` over source audio and writes one 32-bit float BWF per stem into
a destination folder through an injected `StemFileWriter` (`WavStemWriter`
over `common/audiofile`, `FakeStemWriter` in memory). It returns the
import-as-tracks hand-off, a list of `{name, path}`. Cancel or error removes
every file it created. Worker thread only.

## Context

ADR-0165 step 3: stems become new files imported as tracks; the original is
untouched. The separator returns planar buffers in memory; something has to
turn them into files the session can reference, and a cancelled job must not
leave half a set behind for the user to clean up.

## Decision

1. `StemRenderer(StemSeparator&, StemFileWriter&)`; `render(req, out,
   progress, err)` with `StemRenderRequest{source, dest_dir, base_name}`.
2. Files are `<dest_dir>/<base>_<id>.wav` (`id` = `vocals`, `drums`, `bass`,
   `other`, `harmonic`, `percussive`), one per stem the separator filled, in
   `StemKind` order. Track names are `<base> <Label>` ("Take 1 Vocals").
3. Format is 32-bit float BWF, source rate and channel count, so the stems
   sum back to the source within float rounding (tested < 1e-4).
4. A writer must refuse an existing path (`WavWriter::open` already does): the
   renderer never overwrites, so the source and an earlier render survive.
5. Progress: separation maps to 0..0.8, then each written file advances to 1.
   `report()` false cancels at any point, including after the last file.
6. On cancel (`err == "cancelled"`) or a failed write, every file this call
   created is removed and `out` is empty. Pre-existing files are never removed.
7. Hand-off: `std::vector<RenderedStem{name, path}>`. The caller (engine job
   or UI command, a later slice) imports each as a new track at the source
   region's position; this module knows nothing of the session.
8. Edge `common/stems -> common/audiofile` goes in
   `tools/module-deps.d/stems.allow` while `module-deps.allow` is held.

## Rationale

Injecting the writer keeps the rollback and naming rules testable without a
disk; float files keep the null-sum property the separators promise; refusing
existing paths makes "original untouched" structural rather than a convention.

## Consequences

The whole stem set is held in memory during the write (channels × frames × 4
stems × 4 bytes; about 1.4 GB for a 30-minute stereo source at 48 kHz with
six stems). Streamed separation is a later change behind the same interface.

## Alternatives rejected

- Write to temp names and rename at the end: needs a platform rename seam for
  a guarantee that removal on cancel already gives.
- 24-bit integer files: the stems would no longer sum to the source exactly.
- Overwriting on a second render: risks the user's earlier stems.

## Unverified

- Import-as-tracks itself (the next slice consumes the list).
- Behaviour on a full real disk beyond the fake writer's failure path.
