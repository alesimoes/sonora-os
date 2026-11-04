# ADR-0041 — Mix and edit groups: a set of tracks that share chosen attributes

- Status: Accepted (recommended defaults, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Builds on: ADR-0004 (workflow, not binaries), ADR-0009 (operations,
  Compound), ADR-0022 (buses: structure undoable, values not), ADR-0023
- Progress item: `ui.mixer.groups` (mixing, V1)

## Summary

*Navigation only; the sections below are the decision.*

- §1: a group is **structure** in the edit state: `Group{id, name, members,
  attrs, relative, active}`. Two operations, `SetGroup` (create or replace)
  and `RemoveGroup`, both undoable, each one step.
- §2: attributes are six bits: Volume, Mute, Solo, Arm (mix side) and
  Selection, Edit (edit side). A track is in at most one group; a group has
  at least two tracks; at most 64 groups.
- §3: a group's effect is **never stored**. A move on one member is expanded
  by the controlling side into one `Compound` of `SetMix`, so the engine
  sees one revision with every member's new value and the audio thread only
  ever reads its usual per-track atomics.
- §4: Volume is Relative (the same dB added to each member, clamped) or
  Absolute (all to one value); Mute and Solo copy the leader's new state.
  Arm is applied by the engine where it arms (it owns arming).
- §5: session format 19 (a `groups` key only when there is a group; a v18 file
  opens unchanged), window format 16, protocol 48 (one bump shared with ADR-0042
  and ADR-0043, see §5).

## 1. Where a group lives

In `EditState`, beside the buses, so create, edit and dissolve are undo
steps and a replica sees them through the same operations as everything
else. Members are track ids, so a reorder keeps the group. Deleting a track
removes it from its group; a group left with fewer than two tracks is
dropped in the same step, and undo brings both back.

## 2. What is linked

`kGroupVolume`, `kGroupMute`, `kGroupSolo`, `kGroupArm`, `kGroupSelection`,
`kGroupEdit`. `active=false` is Pro Tools' suspend: the group is kept and
links nothing. The model validates and stores all six. This version's
interface offers and honours Volume, Mute, Solo and Arm; **Selection and
Edit are stored but not yet honoured by the interface** (the selection is
one contiguous range today, and a group need not be), so it does not offer
them rather than show a control that does nothing. They are a follow-up.

## 3. How a move is applied

`model::group_follow()` (common/model/groups.h) takes the new mix of the
track the hand is on and returns the other members' mixes. The interface
sends the leader and the followers as one Compound: one revision, one
update, no moment where the group is half moved. A fader ride sends a
`ParameterChange` per member per step (`group_follow_gain()`), then the
release is that one Compound, as a single track's release is one SetMix.
Nothing here is on the audio thread.

## 4. Arm

Arming is the engine's (`track.toggle_arm`). The engine reads the document's
groups with the same `group_peers()` and sets the peers to the leader's new
state, skipping (not refusing) a peer that may not record, such as a MIDI
track. A take in progress refuses it for the group as for a track.

## 5. Versions

Session format 19; the writer emits `groups` only when there is one, so a
session without groups is byte-identical apart from the version. Window
format 16 (the edit state carries `groups`). Protocol 48 (43 to 47 are
held by other branches). These three numbers are shared with ADR-0042 (clip gain)
and ADR-0043 (record modes): the three features landed as one branch, the
session-model train, with a single protocol bump (48), a single session format
bump (19) and a single undo window format (16), and one v18-to-v19 migration.

## Not done here

Selection and Edit attributes in the interface; group-aware sends and
automation-lane writes; groups across several windows' scrolling.
