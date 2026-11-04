# ADR-0042 — Clip gain: a level and a line of breakpoints on the region, in source frames

- Status: Accepted (recommended defaults, taken by the Track 2 agent, 2026-11-04)
- Date: 2026-11-04
- Builds on: ADR-0026 (playlists), ADR-0031 §6 (Consolidate), session-format.md "Versioning"
- Progress item: `ui.editing.clip_gain`

## Summary

- §1: a region has `gain_db` (already there) and a `gain_line` of points
  `{at, db}`; `at` is a **source** frame, so move, trim and split never rewrite
  the line. Between points the gain is linear in amplitude; outside them it is held.
- §2: one operation, `SetRegionGain{playlist, region, gain_db, line}`, one undo
  step. **Session format 19**, **protocol 46** (the numbers next free after the
  open PRs at the time of writing).
- §3: the engine folds the line into the playback plan per region; the audio
  thread multiplies, no allocation (plan points are sized at build time).
- §4: Render Clip Gain is the Consolidate render (ADR-0031 §6) with the line
  applied, a new file and a compound op. The interface cannot yet drive
  Consolidate (control command is a separate PR); `clip.gain_render` refuses
  with that reason until it can.
- §5: interface: gain line and handles over the waveform (+12 to -36 dB),
  Ctrl+Alt+Up/Down nudge by 1 dB, Ctrl+Alt+G opens a numeric panel,
  `clip.gain_point` sets a point at the insertion point. Pointer dragging of
  handles is left for a follow-up.

## Decision

As the summary. A line holds at most 256 points, strictly increasing, within
-144..+24 dB; `gain_line_problem()` is the one validator, used by the codec,
the apply step and the file reader. A version 18 file holding `gain_line` is
refused by its migration, as an older reader would.
