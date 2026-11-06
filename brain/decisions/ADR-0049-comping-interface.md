# ADR-0049 — Comping in the Edit window: take lanes, the swipe gesture, Consolidate

- Status: Accepted (recommended defaults, taken by the comping agent with no
  owner available, 2026-11-04)
- Date: 2026-11-04
- Builds on: ADR-0031 (the model half: a lane is a playlist, a swipe is a
  compound; §6.1 the Consolidate command), ADR-0026 (playlists per track),
  ADR-0016 (a loop take's passes on playlists of their own), ADR-0027 (the
  automation lanes this view sits beside)
- Progress items: `ui.comping.loop_takes`, `ui.comping.lanes`,
  `ui.comping.consolidate`, `ui.comping.exit_test`

## Summary

*Navigation only; the sections below are the decision.*

- §1: take lanes are interface state, like automation lanes: no session
  format change, no undo window change. Protocol 52 carries Consolidate's
  events and the comping commands only.
- §2: the gestures. A drag across a lane selects a range *on the lane*, apart
  from the selection every other edit acts on; **Copy to Main Playlist**
  (button, key) swipes it. A click picks a lane region whole.
- §3: **Quick Swipe** is one switch: copy as the button comes up, with a 10 ms
  join. Off under the Pro Tools keymap, on under Logic's; a toggle holds until
  the profile changes.
- §4: **New comp** and **take folders**: a new empty playlist, the earlier
  active one becomes a lane; the comp names the lane each piece came from.
- §5: **Consolidate**: the window sends the engine the selection; a failure or
  a refusal is a held notice with Dismiss; progress has a Cancel.
- §6: the exit test.

## Context

ADR-0031 gave the model its swipe and the engine its Consolidate. What the
engineer sees and does was left: where the takes are, how to pick the part of
a take to keep, how to see what the comp is made of, and what happens when
Consolidate cannot run. The workflow is Pro Tools' (Playlists view, Copy
Selection to Main Playlist) with Logic's polish (Quick Swipe, the take folder);
both are reproduced as behaviour only (ADR-0004).

## Decision

### 1. Take lanes are a view, not state

A track shows its other playlists as lanes under its row (and under its
automation lanes) when the operator asks: a `≡ n` button on the header,
present only where the track has other playlists, `comp.toggle_lanes`. Which
tracks show theirs is `EditController` state, dropped when a session opens,
as `autolanes_open_` is: it describes how the operator looks, and a stored
flag would need a format bump for a preference. **Session format and the undo
window are unchanged**; the reserved numbers (format 21, undo window 18) are
not taken. Opening on a track with one playlist is refused and says what makes
a lane (a loop take, or New Playlist then record).

Rows grow by `lanes × comp_lane_height` (`ViewState::lanes`), and the lane
height follows the vertical zoom as the automation lanes' does. A lane draws
its regions as the comp does, with their crossfades. The pieces of the comp
that came from a lane tint that lane (`model::comp_origin`, ADR-0031 §4,
computed at draw time), and each such piece of the comp carries `← <lane
name>` and an accent edge, so the comp reads as a list of takes.

### 2. The gesture is a selection first

A drag on a lane (any tool: nothing else a lane offers) is a **lane
selection**: track, lane playlist, range. It is not `Selection`: Clear, Cut or
Separate must not reach the comp through a range drawn on a take, so
`Selection` keeps only an insertion point at the range's start while a lane
range is held. Any other press drops it. **Copy to Main Playlist**
(`comp.copy_to_main`, the lane's **To Main** button, lit while a range is
held) builds `model::comp_range` for the active playlist and submits one
`Compound` labelled "Copy to Main Playlist": one undo step, refused whole as
ADR-0031 says. With none held it is refused and says to drag a range or click
a region. The swiped range then becomes the selection, to audition or loop.

A click without a drag picks the region under it whole (Logic's click on a
take segment). A gesture stays in the lane it began in.

### 3. Quick Swipe

`comp.toggle_quick_swipe`. On, a swipe commits at the release and the joins
ask for a 10 ms centred equal-power crossfade (`CompRange::seam_crossfade`).
Joins the handles cannot take stay cuts, and the notice says how many
("2 of 3 joins kept a hard cut"), held with the track refusals, never a
silent hard edge. The default follows the key map's name (Logic on, else off)
and changes only when the profile does.

### 4. A new comp

`comp.new`: an empty playlist named "<active> comp" (numbered when taken),
made active in one compound ("New Comp"), the lanes opened. The playlist that
was active, usually the best take so far or the takes' own, is a lane: the
workflow is "New comp, then swipe". Playlist switches obey ADR-0026's rule
that no switch happens on a recording track.

### 5. Consolidate

`edit.consolidate` (an interface handler of an engine-domain command, as Cut
is) builds `session.consolidate`'s argument (`api/consolidate_argument.h`) from
the selection: a selected region's span, or the range on the one selected
track, on the active playlist. The reply and the events come back to
`ShellModel`: Running is "Consolidating… n %" with **Cancel**
(`session.consolidate_cancel`); Failed, and a refusal in the reply (a take
recording, a range the model refuses), are a **held notice with Dismiss**, not
the 2.5 s status line; Done and Cancelled clear it (the new region arrives as
an ordinary model update, one undo step). Protocol 52 is the engine's
`session.consolidate`, `session.consolidate_cancel` and `EventKind::Consolidate`
(ADR-0031 §6.1) together with the interface commands of this ADR.

### 6. The exit test

`ui.comping.exit_test` is `ui/qt/tests/test_comping.cpp`, `test_exit`: the
real QML, the Pro Tools profile, no documentation. A vocal with three takes is
comped with the lane button, a drag, To Main, a profile key, Undo, and
consolidated; every step uses a control found where a Pro Tools engineer looks
for it. `docs/comping-a-vocal.md` walks the same path for a reader.

## Rationale

Keeping the lane range apart from `Selection` costs one struct and prevents
the worst failure the feature could have: a Clear that deletes the comp
because a take was selected. Holding the refusals and the consolidate failure
follows the rule that a refusal the operator must see is not the status line.
Not storing the view keeps the session file one thing: what plays.

## Consequences

- `ui/qt`: `edit_comping.cpp` (lanes, lane selection, comp, consolidate
  request), `ViewState::lanes`, `HitZone::TakeLane`, `Drag::Kind::Swipe`;
  EditWindow's `≡ n` button and lane headers; ShellModel's consolidate notice.
- Commands: `comp.toggle_lanes`, `comp.toggle_quick_swipe` (interface),
  `comp.copy_to_main`, `comp.new` (engine-domain, run by the window).
- Keys (`"origin": "sonora"`, neither application documents one): `ctrl+alt+c`
  copy to main, `ctrl+alt+l` lanes, `ctrl+alt+n` new comp, `ctrl+alt+q` Quick
  Swipe, in both profiles.
- Not built: MIDI comping, a collapsed take folder (one row for all lanes),
  Pro Tools' playlist-name dialogs, a swipe across tracks.

## Alternatives rejected

- Persisting which tracks show lanes: a format bump for a preference.
- The lane range as `Selection`: every edit command would need to know it.
- Quick Swipe always on: it changes the audio at every join (ADR-0031).
- Rendering Consolidate in the window: it has no files (ADR-0031 §6.1).
