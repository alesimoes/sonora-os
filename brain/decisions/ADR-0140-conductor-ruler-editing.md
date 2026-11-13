# ADR-0140 - Conductor-ruler editing: pure gestures over the tempo map in `ui/conductor`

- Status: Accepted
- Date: 2026-11-09
- Item: `time.tempo_map`
- Relates to: ADR-0050 (tempo operations), ADR-0115 (groove module hook), ADR-0007 (module DAG)

## Summary

The conductor ruler lets the engineer grab a tempo mark, drag it up or down to change the tempo, delete it,
and pick a meter for the bar. The gesture maths is a Qt-free module `ui/conductor` that depends on
`sonora_model` only: `hit_test`, `drag_tempo`, `apply_tempo_drag`, `remove_mark`, `apply_meter`,
`meter_choices`. Every change goes through `tempo_edit.h` (`tempo_set_at`, `tempo_remove_at`,
`meter_set_near`), so the map is always one `tempo_map_problem` accepts. No change to `model.h`,
`operations.h`, the session format, the protocol or `contracts/`.

## Context

`time.tempo_map` needs the ruler's editing behaviour. A ramp is a staircase of ordinary entries
(ADR-0050), so every step is a mark of its own. The view code (QML, a later task in `ui/qt`) must not
carry tempo arithmetic: it would be untestable without Qt and could write a map the format refuses.

## Decision

1. `ui/conductor` (`sonora/ui_conductor/conductor.h`), static library `sonora_ui_conductor`, links
   `sonora_model` only (allow line `ui/conductor -> common/model`).
2. `hit_test(map, rate, frames_per_pixel, x)` returns the index of the nearest entry within 6 px
   (inclusive), else none. `x` is pixels from frame 0; scrolling is the caller's.
3. `drag_tempo(map, index, dy_pixels, whole_bpm)`: the entry's tempo at drag start plus 0.1 bpm per
   pixel up, snapped to 0.01 bpm (whole bpm with the modifier), then clamped to `[kMinBpm, kMaxBpm]`
   (the format's bounds, `tempo_map_problem`). The caller passes the map from before the drag and the
   total `dy`, so a drag is idempotent and one undo step.
4. `apply_tempo_drag`, `remove_mark`, `apply_meter` call `tempo_set_at`, `tempo_remove_at`,
   `meter_set_near` and return false with the reason, the map untouched. The first entry cannot be
   removed; a meter lands on the nearest bar line.
5. `meter_choices()`: beats 1..32, note 1, 2, 4, 8, 16 (the menu; the model allows more).
6. A drag of one ramp step moves that step only; the rest of the staircase stays, and the map stays valid.
7. Build hook: the root `CMakeLists.txt` is owned by `time.elastic` for now, so `ui/groove/CMakeLists.txt`
   carries one guarded `add_subdirectory(${CMAKE_SOURCE_DIR}/ui/conductor ...)` before its return, as
   groove avoided `ui/qt`'s CMake. The tests carry the label `ui` set in `ui/conductor/CMakeLists.txt`.
   Once `time.elastic` merges, the hook moves to the root and the groove line goes.
   `ui/conductor/CMakeLists.txt` also adds `qt/` if it exists, for the UI task.

## Rationale

Pure functions test without a display, and the format check stays in one place. The bounds come from the
model rather than a copy, so a bound change cannot leave the ruler writing invalid maps.

## Consequences

- A later `ui/qt` task binds the ruler to these functions and owns the screenshots.
- Dragging a ramp step by hand leaves the staircase uneven; the ramp tool is the way to reshape it.
- Snapping to 0.01 rewrites a ramp step's unrounded tempo on its first drag.

## Alternatives rejected

- Gesture maths in QML/Qt code: untestable headless, could bypass `tempo_edit`.
- A new `operations.h` operation for a tempo drag: the `Operation` variant is a compile-time trap and
  the existing ones suffice.
- Dragging a whole ramp as one unit: a different gesture (`tempo_ramp`), not this slice.

Index line (for the PR body): `- [ADR-0140](decisions/ADR-0140-conductor-ruler-editing.md) - Conductor-ruler editing: pure gestures in ui/conductor over tempo_edit`
