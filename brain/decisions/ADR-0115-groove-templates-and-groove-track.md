# ADR-0115 - Groove templates and the groove track: a pure timing value applied as region moves

- Status: Accepted
- Date: 2026-11-08
- Item: `time.groove`
- Relates to: ADR-0009 (immutable edit state, compound operations), ADR-0050 (tempo operations)

## Summary

A groove template is a small pure value in its own module `common/groove` (links `sonora_model`): per grid slot, a timing offset (fraction of a
slot) and a gain factor. It comes from a preset (swing), or is extracted from the onsets of a reference
("groove track") region. Applying it moves audio regions onto the template's slots with a strength
percentage, as one Compound of existing `MoveRegion` operations, so it is one undo step. No new
operation, no protocol change, no session format change. Templates the user saves live in a preferences
file, not in the session.

## Context

BACKLOG `time.groove` (Logic: Groove Track, groove templates). MIDI is out of v1, so the groove acts on
audio regions. Regions are the unit the timeline can move; slicing audio at transients is Beat Detective
territory and is not in this feature. The `Operation` variant is a known compile-time trap and the audio
thread must not learn a new concept.

## Decision

1. `common/groove/` (`sonora/groove/groove.h`, `src/groove.cpp`; own CMakeLists, wired by one root `add_subdirectory`, so the shared model CMake is not touched): `GrooveTemplate {slots_per_beat, vector<GrooveSlot{offset, gain}>}`
   (at most 64 slots, offsets within +-0.5 slot), `groove_swing(percent, subdivision)`,
   `groove_from_onsets(map, rate, onsets, slots_per_beat, why)` (onsets folded onto the grid, median offset
   per slot, empty slot = 0) and `groove_moves(map, rate, template, regions, strength, why)` returning
   the `(region, new_position)` list. Pure, off the audio thread, uses `MusicalMap`.
2. A region is moved to `slot_frame(nearest slot) + offset * strength`, the nearest slot being that of
   its start. A move that would overlap a neighbour (playlists never overlap, ADR-0009) is skipped and
   counted, never overwriting; the result tells the user how many were skipped.
3. The UI applies the list as one Compound of `MoveRegion` (precedent: strip silence). Extraction of the
   reference's onsets sits behind an injected `OnsetSource` (a peak-envelope picker for now), so a later
   beat-detector pass plugs in without touching the model.
4. The command is the existing text entry `tempo.edit` with new words `groove`; `contracts/command.h`
   is not edited. User templates are JSON (`common/json`, schema version) in the preferences directory.

## Rationale

Reusing `MoveRegion` keeps undo, replication and the plan swap unchanged: the engine sees ordinary
moves. A value type is testable without Qt or audio. Preferences storage avoids a session format bump
for a convenience feature.

## Consequences

- Groove acts on whole regions: a region holding several hits keeps its internal timing. Users who
  want hit-level timing need region separation first (follow-up: slice at onsets, then groove).
- Skipped moves are visible, not silent. Strength 0 is a no-op and is refused with a reason.
- Templates do not travel with a session. Accepted for now.

## Alternatives rejected

- A new `ApplyGroove` operation: grows the variant list, protocol and codec for no gain.
- Storing templates in the session: format bump and migration for little value.
- Real-time groove on the audio thread (playback-time offsets): puts a timing transform on the RT path,
  and the timeline would stop matching the plan.
- Moving overlapping regions and overwriting neighbours: can destroy audio.

## Unverified

Whether whole-region moves give a musically useful result on real loops (assumption). The researcher
should try swing 54-66 % on a few drum loops after merge.
