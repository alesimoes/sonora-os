# ADR-0116 - Arrangement sections: range markers, moved and copied as one Compound of existing operations

- Status: Accepted (recommended defaults)
- Date: 2026-11-08
- Item: `time.arrangement`
- Relates to: ADR-0009 (compound operations), ADR-0052 (clip groups), ADR-0115 (same pattern: pure plan, one Compound)

## Summary

An arrangement section is a marker with `length > 0` (a range marker, the "selection marker" `marker.add`
already makes). Moving or copying a section is a pure function in `common/model`
(`arrangement.h`) that returns one `Compound` of existing operations (`SplitRegion`, `PlaceRegion`,
`MoveRegion`, `RemoveRegion`, `ShiftRegions`, `MoveMarker`, `AddMarker`), so it is one undo step. No new
operation, no `Operation` variant growth, no protocol change, no session format change.

## Context

BACKLOG `time.arrangement` (Logic: arrangement track and global tracks; Pro Tools: rulers). The model has
`Marker{id, position, name, length}` and Add/Move/RemoveMarker. A new marker kind would change the session
format (the daemon reserved none) and the `Operation` variant is a known compile-time trap.

## Decision

1. Sections are the range markers of `EditState::markers`; no new state. Point markers (memory locations)
   are not sections. Selection markers also show as sections; a `kind` field is a later format bump if
   owners want them apart.
2. `arrangement.h/.cpp`: `SectionEdit {section, destination, mode}` with mode `Copy` or `Move`, and
   `plan_section_edit(snapshot, edit, next_id) -> SectionPlan` (C++17 has no `expected`: a `Compound`
   and a `refusal` string, `ok()` when the refusal is empty). Both modes are insert-style: material at or
   after `destination` shifts later by the section length (`ShiftRegions`), the copy fills the gap
   (`PlaceRegion`, new ids, no clip group). `Move` moves the section's own regions into the gap
   (`MoveRegion`, ids kept), then closes the gap it left (`ShiftRegions` by -length); a Move to a
   destination after the section lands it at `destination - length`, before what was at the destination.
   Regions straddling the section's start, end or the destination are split first (`SplitRegion`);
   every audio track shifts, so tracks stay aligned; the active playlist of every audio track only.
   Markers at or after the destination shift with the material (`MoveMarker`); markers inside the
   section are copied with it (`AddMarker`, `Copy`) or move with it (`Move`); a copy of the section
   marker itself is added at the destination (`Copy`) or the marker moves (`Move`).
   `section_markers()` lists the sections; `kSectionEditLimits` is the text of Decision 5.
3. Ids: the planner takes ids from `next_id` strictly increasing in member order, as `Compound`
   requires. Every split is decided at plan time, so no id is conditional: `PlaceRegion` and
   `MoveRegion` carry `split_right_id = 0`, which makes `apply()` refuse instead of overwriting if the
   gap were ever not empty. The planner applies each member to a working copy as it builds, so a plan
   it returns is one `apply()` accepts.
4. Refusals (a message, nothing applied): no such marker, or a point marker; destination inside the
   section itself, or for `Move` at either of its ends (it is already there); destination before frame
   0, or material pushed past the end of the timeline; section has no audio and no markers; Compound
   would exceed `kMaxCompoundMembers` (256) members (message names the count, worked out before
   anything is built); MIDI tracks are skipped, never an error.
5. Automation lanes, tempo/meter map entries, plugin state and crossfades across the cut are NOT moved
   in v1 (crossfades drop, as `ShiftRegions` already defines). The UI says so in the command message.
6. UI: a Sections list in `MemoryLocationsWindow.qml` (rows of range markers) with Move to... and Copy to...
   (a counter field for the destination, in the main counter format), `EditController::arrangement_*`
   in `edit_arrangement.cpp`. No new command in `command.h`, no `EditWindow.qml`, no `Main.qml` edit:
   those belong to other features. The conductor-ruler arrangement lane with dragging is a later item.

## Rationale

Reusing the operations keeps undo, replication and the codec untouched and is the proven path (ADR-0115,
strip silence). A pure planner is testable without a UI. Insert semantics match what arrangers expect and
never overwrite material, so no take is lost by a drop.

## Consequences

Big sections on dense sessions are refused by the 256 cap; a chunked variant would need a larger cap and is
a follow-up. Automation not following a moved section is a visible limit.

## Alternatives rejected

- A `Section` type with its own operations and a format bump: format not reserved, variant growth, and
  every consumer of the session file changes.
- Overwrite (replace) semantics at the destination: destroys material silently.
- Name-prefix convention to tell sections from markers: invisible coupling, breaks on rename.

## Unverified

- Compound size on real sessions (tracks x regions in range) against the 256 cap: researcher to count
  members for a 32-bar section on a 24-track, 6-regions-per-track session with `measure_compound`.
- Whether range markers made by selection should be excluded: owner preference, default is include.
