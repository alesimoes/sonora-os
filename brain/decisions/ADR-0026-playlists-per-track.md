# ADR-0026 — Playlists per track: duplicate, rename, delete unused, and the take's playlist

- Status: **Accepted**, 2026-10-31, by the coordinator on the recommended
  defaults below.
- Date: 2026-10-31
- Proposed by: rt-engineer, backlog item `ui.comping.playlists`.
- Extends: ADR-0009 (operations, undo), ADR-0018 (the session folder owns
  its media), ADR-0023 (track structure; the same refusals during a take).
  Session format unchanged (11); control protocol 31 (three new operation
  kinds; 29 and 30 are held by PRs in flight).

## Summary

*Navigation only; the sections below are the decision.*

- §1: three new operations. `DuplicatePlaylist` copies one of a track's
  playlists as a new one; `SetPlaylistName` renames; `RemovePlaylist`
  takes off a playlist that is not the active one. New Playlist stays
  `AddPlaylist` + `SetActivePlaylist` in one compound; Delete Unused
  Playlists is one `RemovePlaylist` per playlist in one compound. Each
  command is one undo step.
- §2: Pro Tools' names: `<track>.<nn>`, the lowest free number from 01,
  unique across the session. The interface keeps names unique; the model
  does not require it, so older sessions still load.
- §3: the engine plays the active playlist. Choosing another one is a new
  plan, heard from the first block after the swap. Nothing new runs in
  `process()`.
- §4: while a take is starting or closing, no track may choose another
  playlist. While a take records, an armed track may not. This covers an
  operation, a compound member, an undo and a redo. A take lands on the
  playlist that was active at Record.
- §5: media is in use when the pool names it. A playlist that is inactive
  or removed never takes its sources out of the pool. The orphan offer
  never lists them, and saving keeps them.

## Context

A playlist is an alternate set of regions on one track (Pro Tools
Reference Guide, "Playlists"). The model has had several playlists per
track, `AddPlaylist` and `SetActivePlaylist` since ADR-0009. The session
file writes every playlist and which one is active. The plan builder
plays only the active one. Duplicate, rename and delete were missing, and
so was a rule for a switch during a take.

## Decision

### §1 Operations

`DuplicatePlaylist{track, source, playlist, name}`: the copy's id is
`playlist`. Its regions take the next ids, in position order
(`duplicate_playlist_ids()`), so `apply()` stays pure. Crossfades follow
their regions. Audio sources and MIDI sequences are shared, not copied
(ADR-0014 §1). The active playlist does not change: the interface sends
`SetActivePlaylist` in the same compound, as Pro Tools does.
`DuplicateTrack` numbers its copies with the same helper.

`SetPlaylistName{track, playlist, name}`: the name must be non-empty
UTF-8.

`RemovePlaylist{track, playlist}`: the active playlist cannot be removed.
The sources of its regions stay in the pool (ADR-0009 §3), so undo brings
the playlist back with its audio.

These are separate operations, not compounds of region ops: a duplicate
of a long playlist would pass the compound limit (256), as in ADR-0023 §1.

### §2 Names

`next_playlist_name()` returns `<track name>.<nn>`, two digits or more,
using the lowest number no playlist in the session already has.
`playlist_name_taken()` lets the rename dialog refuse a duplicate name.

### §3 Switching is a plan swap

The plan builder already reads `TrackEdit::active_playlist`. A switch is a
document change: the control thread builds a plan, the disk reader
prefetches it, and the new plan is published. The audio thread plays the
old plan, exactly, until the swap and the new one, exactly, from the next
block. So the switch is sample-accurate at a block edge whatever the
block size, with no allocation and no miss. `test_playlist_switch` checks
this with fixed and irregular block sizes and the heap counter.

There is no crossfade at the switch: a ramp would need a second voice per
track in the plan. A declicked switch is a
BACKLOG line, not part of this decision.

### §4 Refusals during a take

A take goes onto its track's active playlist once its files are in
(closed_take.cpp). If that playlist changed between Record and then, the
take would land on a playlist the operator did not record onto.

The refusals are the same class as ADR-0023 §5:

- A take about to start: every switch is refused, "a take is about to
  start: choose another playlist before Record or after Stop".
- A take closing: every switch is refused, "take N is still closing:
  choose another playlist once it is in the session".
- A take recording: a switch on an armed track is refused, "\"Vox\" is
  recording: stop recording before choosing another playlist". A track
  that is not armed may switch.

The engine asks this for each compound member, and for the state an undo
or a redo would restore (`active_playlist_changes()`). FakeEngine gives
the same answers. Conformance checks the refusals on both engines, and
checks that the real engine's take lands on the playlist active at
Record.

Adding, renaming or removing an inactive playlist during a take is
allowed. None of them changes where the take lands.

### §5 Media referenced by any playlist is in use

The orphan offer (ADR-0018 §6) compares the audio folder with the pool,
not with the active playlists. A source that only an inactive playlist
uses, or that only a removed playlist used, stays in the pool. It is
never offered, never placed twice and never deleted. MIDI sequence files
are counted by every playlist's regions (`named_sequences()`). Tests:
`orphan_offer` (`inactive_playlist_media_is_not_orphaned`, through a save
and reload) and `model_playlists`.

## Consequences

- Protocol 31: a version-28 peer's decoder refuses the three new kinds.
- Session format unchanged: the file already holds every playlist.
- Comping lanes and Promote to Main (`ui.comping.lanes`) build on these
  operations.

## Alternatives rejected

- **Pin the playlist in the take** (record onto the playlist active at
  Record even if the operator switched): the take would land somewhere
  the operator is not looking.
- **Delete media of removed playlists**: data loss on undo, and a
  contradiction of ADR-0018 (the folder keeps its media until an explicit
  cleanup).
