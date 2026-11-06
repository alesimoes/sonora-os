# ADR-0050 — Tempo editing: ramps as staircases, tempo as session settings, a tempo track

- Status: Accepted (recommended defaults, taken by the Track 3 agent, 2026-11-04)
- Date: 2026-11-04
- Builds on: ADR-0014 §2 (musical time), session-format.md "tempo"
- Progress items: `time.tempo_map` (finish), `time.tempo_edit`

## Summary

- §1: a ramp is a **staircase of ordinary tempo entries**, two to a beat at
  the mean tempo, 2 to 128 steps, even in time. The session format, the
  `MusicalMap` conversions (exact integers), the click plan and the grid read
  it unchanged; there is no new field and no float in a conversion.
- §2: the operations live in `common/model/tempo_edit.h`: set a tempo, remove
  a change, ramp, scale a range (double and half time are factors 2 and 0.5),
  set a meter on the nearest bar line. Each returns a whole new map checked by
  `tempo_map_problem`, or refuses by name and leaves the map alone.
- §3: tempo is **session settings**. An edit is a `SetSettings` with the map
  replaced, so no operation, protocol or session format changes (numbers 53,
  22 and undo window 19 were reserved and are not used). Audio stays where it
  is in samples; only the bars and the click move.
- §4: undo. Settings are outside the edit undo (the loop and punch ranges are
  too), so tempo edits have **Undo Tempo / Redo Tempo** on a stack of the
  interface's own (64 steps), cleared when the session is replaced or when the
  map is no longer the one a step made. Folding tempo into the engine's undo
  window is a follow-up that needs the format bump.
- §5: interface: `tempo.edit` (Edit > Tempo…: "96", "120>90" over the range,
  "x2", "3/4", "remove"), `tempo.undo`, `tempo.redo`, `tempo.track` show the
  tempo track under the Meter ruler. Interface-domain commands: nothing is sent
  to the engine but the `SetSettings`.

## Why a staircase

A continuous ramp needs a square root or a logarithm in the frame-to-tick
conversion, which breaks the exact-integer property that makes the plan
builder, the take resolver and the grid agree to the frame (midi.md §2). A
step every half beat is below what a player follows, and the map stays
something a Pro Tools or Logic user can read and edit entry by entry.

## Consequences

- A meter change holds through the tempo-only entries after it (a ramp's steps
  carry it), up to the next entry with another meter; removing it gives them
  the previous meter back.
- A ramp over a stretch that holds a meter change is refused; ramp each side.
- Dragging the tempo track's handles with the pointer is not in this slice; the
  dialog and the command are the editing path.
