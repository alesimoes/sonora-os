# ADR-0048 — VCA masters and folders in the Mix window

- Status: Accepted (recommended defaults, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Builds on: ADR-0004 (workflow, not binaries), ADR-0009 (operations,
  Compound, mix outside undo), ADR-0022 (buses: structure undoable, values
  not), ADR-0023 (the positional engine; a deleted track leaves what held it)
- Progress items: `ui.mixer.vca`, `ui.mixer.folders` (mixing, V1)
- Numbers (coordinator, 2026-11-04): session format 20 (a v19 -> v20
  migration on top of the session-model train's v19), window format 17,
  protocol 51 (48 is the session-model train, 49 bounce, 50 preferences).
  Renumbered from the placeholders 19, 16, 46 and ADR-0042.

## Summary

*Navigation only; the sections below are the decision.*

- §1: a VCA master is **structure** in the edit state, `Vca{id, name,
  members}`; its fader and mute are a **mix value**, `VcaMix{vca, gain_db,
  mute}`, saved and not undone. Five operations: `SetVca`, `RemoveVca`
  (undoable), `SetVcaMix` (not), `SetFolder`, `RemoveFolder` (undoable).
- §2: a VCA carries no audio. The engine multiplies each member's fader by
  one factor per track, the sum of its VCAs' dB (0 for a muted VCA); the
  fader, its automation and the pan are untouched, so the fader still shows
  and plays what was written.
- §3: the factor is a new per-track atomic, `Track::vca_gain()`, read once
  per block and ramped with the fader. It is written by the control thread
  from the document, goes with its track at a reorder in the plan's mix
  (`PlanTrackMix::vca`), and follows post-fader sends, not pre-fader ones.
- §4: a folder is `Folder{id, name, members}`: a named set of tracks, a track
  in at most one. Mute and solo on a folder are one Compound of `SetMix`
  over its members. Whether a folder is shown closed is the window's, not
  the session's, and goes back to open when a session opens.
- §5: session format 20 (`vcas`, each with its dB and mute, and `folders`,
  only when there are some), window format 17, protocol 51. Commands
  `vca.new`, `vca.delete`, `folder.new`, `folder.dissolve`.

## 1. Where they live

`EditState::vcas` and `EditState::folders`, beside the buses, so making,
changing and removing either is one undo step and a replica sees them
through the operations like everything else. `MixState::vcas` holds the
fader and the mute, keyed by VCA id, outliving an undone `SetVca` as a
track's do; a VCA with no entry is at 0 dB and unmuted.

Members are track ids, so a reorder keeps them. Deleting a track removes it
from every VCA and folder in the same step; undo brings both back. A VCA or
folder with no member is allowed (a VCA being rebuilt, a folder emptied):
unlike a group it links nothing that could be left half-linked.

A track may be on several VCAs; their dB add, as Pro Tools' nested VCA
assignments do. A track is in one folder at most; making a folder over a
track that is already in one is refused by name and nothing is changed.
Limits: 32 VCAs, 64 folders.

## 2. What the engine does

The factor for a track is `10^(sum of its VCAs' gain_db / 20)`, clamped as
every gain is (`gain_of_db`), and 0 when any VCA that holds it is muted. It
is applied as one more multiplier where the track's `open` state (mute and
solo) already is, so:

- a static fader ramps to `fader * vca` across the block as it does for a
  fader move;
- an automated fader plays its lane times the factor, and a change of the
  factor starts the same crossfade a mute does (`track_open_last_`);
- a post-fader send follows it, a pre-fader send does not (Pro Tools'
  VCAs do not move pre-fader sends).

Nothing on the audio thread allocates, locks or waits: the factor is a
relaxed atomic float, like the gain. `PlaybackController` writes it in
`apply_mix()` for every track, in `fill_track_mix()` for the plan that moves
tracks (so the factor goes with the document track at a reorder, as the
fader does, ADR-0023 §4), and `adopt_track_mix()` writes it with the rest.
The server treats a change of a VCA's members or values as a mix change.

A VCA ride has no live parameter of its own: each step is a `SetVcaMix`,
which the engine applies as it goes, as the monitor bus rides today. A
`ParamId::VcaGain` is a follow-up if a revision per step proves heavy.

Not done here: VCA solo, a VCA of VCAs, VCA automation lanes, a folder's
routing (a summing folder that is a bus), nesting folders.

## 3. The interface

The VCA strips are at the right of the aux returns, each with a fader, a
mute, a "Set" that makes it hold the selected tracks, and a delete; the
tracks it holds wear its name in a tag. `+ VCA` and `Folder` in the views
row make one of the selected tracks. A folder shows a tag on each member
(a click closes it); closed, it is one card where its first track was, with
mute, solo, Open and Ungroup, and its tracks' strips are not drawn. A
refusal (a track already in a folder, no track selected, a duplicate name)
is a held notice, not the 2.5 s status line.
