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
   `plan_section_edit(snapshot, edit, next_id) -> expected<Compound, Refusal>`. Both modes are
   insert-style: material at or after `destination` shifts later by the section length (`ShiftRegions`),
   the copy fills the gap. `Move` then removes the source range and closes the gap (`ShiftRegions` by
   -length). Regions straddling a boundary are split first; audio playlists of the active playlist of every
   audio track only. Markers at or after the destination shift with the material; markers inside the
   section are copied with it (`Copy`) or move with it (`Move`); the section marker itself is re-added.
3. Reserved ids: the planner takes ids from `next_id` strictly increasing, reserving every conditional one
   (`split_right_id`), as `Compound` requires.
4. Refusals (a message, nothing applied): destination inside the section itself; section end beyond the
   timeline; section has no audio and no markers; Compound would exceed `kMaxCompoundMembers` (256) members
   (message names the count); a MIDI-only session part is skipped, never an error.
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
