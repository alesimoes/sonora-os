# ADR-0022 — Track structure: delete, duplicate, and the positional engine

- Status: **Accepted**, 2026-10-27, by the coordinator on the recommended
  defaults below.
- Date: 2026-10-27
- Proposed by: rt-engineer (model squad), backlog item `daw.model.tracks`
  ("Track operations and types").
- Extends: ADR-0009 (operations, undo), ADR-0012 §6 (insert values outside
  undo). Session format unchanged (10); control protocol 26 (two new
  operation kinds).
- Numbering: follows ADR-0021.

## Summary

*Navigation only; the sections below are the decision.*

- §1: `RemoveTrack` and `DuplicateTrack` are operations of their own, not
  compounds of existing ones: a duplicate of a track with a few hundred
  regions would pass the compound limit (256).
- §2: a duplicate is a new track. Every object it holds gets a new id,
  rising from the op's `track` id in a fixed order, so `apply()` stays pure.
  Audio sources and MIDI sequences are shared, not copied. The default set
  is Pro Tools' Duplicate Tracks dialog's: the active playlist, the
  inserts with their values, the mix; alternate playlists on request.
- §3: delete is edit-undoable. The track's mix entry and its inserts'
  values stay in `MixState` as an undone `AddTrack`'s do, so undo brings
  it back exactly. Its sources stay in the pool.
- §4: engine track i plays document track i. A structure change therefore
  moves each track's fader, pan, mute, solo, monitor mode, record arm and
  capture input to another engine track, and they move at the plan swap:
  the new plan carries them, the audio thread uses the plan's values until
  the control thread has rewritten the per-track atomics just after
  publishing it. The capture input is the track's `mix.input`, read from an
  atomic per engine track.
- §5: while a take is open, requested or closing, a change (apply, undo or
  redo) that would delete or move an armed track, or one whose take is
  still closing, is refused by name. So is a track past the engine's track
  count.

## Context

The model had `AddTrack`, `SetTrackName` and `ReorderTracks`; nothing
removed or duplicated a track. The engine maps document tracks to its
fixed set of engine tracks by position (`PlaybackController::show`,
`build_plan`), and every engine track captured the hardware input of its
own index whatever the document said. Arming lives on the engine track.
So a reorder already moved the arm and the input to whichever document
track landed on that position: "Vox", armed on input 1, moved down one
row, would record input 2 into "Kick".

## Decision

### §1 Two operations

```
RemoveTrack    { track }
DuplicateTrack { source, track, name, all_playlists = false, inserts = true }
```

Both are undoable members, so they also ride compounds. A compound of
`AddTrack`, `AddPlaylist`, `PlaceRegion` ... was the alternative: no new
operation kind and no protocol bump, but it fails for a track over about
250 regions, and its size grows with the track while this one is a few
words whatever the track holds.

### §2 Identity across a duplicate

The copy is a new track, placed directly below its source, as both Pro
Tools and Logic place it. Ids: `track` is the copy's; then, for each
playlist copied, in the source's order, the playlist and then each of its
regions in position order; then each insert copied, in slot order. Every
id must be at least `next_id`, and `next_id` becomes the last one plus 1,
so the sender reserves `duplicate_track_ids()` ids. Crossfades follow their
regions' new ids. A MIDI region keeps its `shared_ptr` to the sequence
(copy-on-write, ADR-0014 §1): a duplicate costs the undo budget a few words
per region and sends no notes.

Copied with the track: name (the sender's, Pro Tools' `<name>.dup1` by
default), kind, channels, the active playlist (all playlists with
`all_playlists`), the inserts and their values (bypass, parameters, state
reference; a state file is named by its digest, so two inserts share one
safely), and the mix: input, monitor mode, fader, pan, mute, solo safe and
MIDI routing. Solo is not copied: a duplicate made while its source is
soloed would otherwise solo a second track the operator did not ask for.
The source's arm is not copied either (§4: arm belongs to the track id).
Playlist names are the copy's name and the source playlist's suffix after
the source track's name (`Vox.02` becomes `Vox.dup1.02`), or the
playlist's own name when it does not start with the track's.

Sends, automation and group membership are copied by this rule when they
exist: they belong to the track, so the copy gets its own.

### §3 Delete

`RemoveTrack` takes the track out of the edit state. As for an undone
`AddTrack`, its mix entry stays (not saved: `same_document` and the writer
skip mix entries of tracks that do not exist) and its inserts' values stay
until no retained step holds the insert (`History::prune_insert_values`),
so undo restores the fader and the plugins where they were. Its sources
stay in the pool, `recorded_on_track` naming a track that may come back.

Routing on delete: today a track's only destination is the monitor bus.
When buses and sends land (`ui.mixer.io`, `ui.mixer.sends`), a send is part
of its track and goes with it; a send *to* a bus that is deleted is a
question for that ADR, not this one.

### §4 The positional engine

Engine track i plays document track i in display order. Plug-in chains and
delay lines were already keyed by track id (`PlanContinuity`), so a reorder
hands no track another's chain. The per-track mix was not: it lives in
`Track`'s atomics, by position, and `ReorderTracks` rewrote them at once,
while the old plan went on playing the old positions until the new one was
resident and swapped in. Each moved track was heard with another's fader
and pan for that time (tens of milliseconds and more while the reader
loads), a defect on main before this ADR.

The fix keeps engine tracks positional and moves the mix with the plan:

- **Plan-carried mix.** When `PlaybackController::show()` builds a plan
  whose track order differs from the one the atomics hold, it gives the
  plan a `mix_serial` and fills `track_mix[e]` for every engine track:
  gain, pan, mute, solo, solo safe, monitor mode, arm, input. Nothing
  positional is written meanwhile (`tracks_moving()`); a mix asked for in
  between is kept and written later.
- **The audio thread.** For each engine track, while `Track::mix_serial()`
  is not the playing plan's `mix_serial`, it takes the mix from the plan;
  otherwise from the atomics. On adopting such a plan it snaps the fader
  ramps and the monitor ramps, because what an engine track ramped from
  belonged to the track that was on it. The bus sum is then exactly the
  same across the swap.
- **Catching up.** Right after `pump()` publishes the plan, the control
  thread writes the plan's values to the atomics and stamps each track
  with the serial (release; the audio thread reads it with acquire), then
  writes any mix kept meanwhile.
- **Arm.** The plan's arm for an engine track is the arm the document
  track now on it had where it was: the arm goes with its track. A deleted
  track's arm goes; a duplicate starts disarmed. Arming commands are
  refused while tracks are moving, and a fader ride step is dropped, since
  both name an engine track by position.
- **Input.** `Track::input()` is an atomic the audio thread reads once per
  block for metering, capture and input monitoring, set from the document
  track's `mix.input[0]` (the engine's own index when no document track is
  on it). It is not written for an armed track while a take runs, so a
  take's file never changes input part way.

### §5 Refusals

While a take is open, requested or closing (`Session::folder_change_refusal`,
`record_requested`), the engine refuses a member, an undo or a redo that
would delete an armed track, move one to another position, or do either to
a track whose take is still closing: the take's files are placed on the
track at their engine position when they close. The words name the track
and say what to do: `"Vox" is recording: stop before deleting it`.
Outside a take nothing is refused for arming: the arm goes with the track
(§4) or with its deletion.

`AddTrack` and `DuplicateTrack` past the engine's track count are refused
by name (the plan builder would refuse the document anyway, with playback
left on the old plan). FakeEngine applies the same rules, so the
conformance suite runs them on both.

## Consequences

- Protocol 26: a version-25 peer's decoder refuses the two new kinds.
- Session format stays at 10: the states these operations make are ones
  format 10 already holds.
- The engine now honours `mix.input` for capture; sessions written so far
  carry input i on track i, as the engine assumed.
- VCA masters and folder tracks are not track kinds yet (`ui.mixer.vca`,
  `ui.mixer.folders`); aux and master tracks wait for buses
  (`ui.mixer.io`); New Track for MIDI and instrument kinds waits on
  MIDI playback and instrument hosting (`midi.instrument_tracks`).

## Alternatives rejected

- **Duplicate as a compound.** No protocol bump, but bounded by the
  compound limit and sized by the track's contents (§1).
- **Engine tracks keyed by document id.** Arm, meters and commands address
  tracks by display index across the contract; keying engine tracks by id
  would change the state block's meaning for every client, and every
  command and the publisher would translate rows to engine tracks.
- **Write the atomics when the swap is seen.** The control thread learns of
  the swap after the audio thread makes it, so at least one block would
  play with the wrong mix, and the order of publish and write leaves one
  wrong block either way. Only a mix inside the plan swaps with it.
- **Refuse every structure change during a take.** Safe, but a mix
  engineer adding a track while a take rolls on others is ordinary; only
  the tracks a take is writing are pinned.

## Open questions

None for the owner.
