# ADR-0031 — Take lanes and swipe comping: a lane is a playlist, a swipe is a compound

- Status: Accepted (recommended defaults, taken by the Track 2 agent with no
  owner available, 2026-11-03)
- Date: 2026-11-03
- Builds on: ADR-0026 (playlists per track), ADR-0016 §3 (a loop take's
  passes on playlists of their own), ADR-0009 update 2026-10-14 (compound
  operations), ADR-0021 (crossfades on short handles)
- Progress item: `ui.comping.lanes` (the model half); `ui.comping.consolidate`
  names what is left

## Summary

*Navigation only; the sections below are the decision.*

- §1: a take lane is one of the track's playlists. Nothing new in the
  document: **no session format bump, no protocol bump**.
- §2: a swipe is Pro Tools' Copy Selection to Main Playlist: a `Compound`
  of the members the model already has, built by `model::comp_range()`,
  one undo step. The target range plays exactly what the lane plays there,
  its silence included.
- §3: seams are hard cuts by default, as in Pro Tools. A seam crossfade
  length asks for Logic's Quick Swipe crossfades, fitted to the handles and
  to half of each region; a seam that cannot take one stays hard and is
  counted, so the interface can say so.
- §4: which lane a comped region came from is computed
  (`model::comp_origin()`), never stored.
- §5: Consolidate: the engine renders, `model::consolidate_range()` replaces
  the range with one region (one `PlaceRegion`, no version bump).
- §6: the entry point of Consolidate (update of 2026-11-04): the command
  `session.consolidate`, run on a worker, progress and result as
  `Consolidate` events, `session.consolidate_cancel`, refused while a take
  is open; **protocol 43**. Left for later: take folders' collapsed view,
  MIDI comping.

## Context

Comping is the core of the studio vocal workflow: several takes of a
phrase, the best piece of each assembled on one playlist. Pro Tools shows a
track's playlists as lanes under it ("Playlists" view) and copies a
selection from a lane to the main playlist with a click or a key
(Copy Selection to Main Playlist; Avid's Pro Tools Reference Guide,
"Playlists" and "Track Compositing"). Logic stacks takes in a take folder
and builds the comp by swiping across them (Quick Swipe Comping; Logic Pro
User Guide, "Comp your audio recordings"), with short crossfades at the
joins.

The model already has every piece: playlists per track with their own
regions (ADR-0026), loop passes recorded on playlists of their own
(ADR-0016), region placement that overwrites, trims, splits, crossfades,
and compounds that make many members one undo step. What it lacks is the
rule that turns "this range from that lane" into members, written once so
the interface, the TUI and any later script build the same edit.

## Decision

### 1. A take lane is a playlist

The lanes of a track are its playlists other than the target (usually the
active one). A loop take's passes already land there; a punched take lands
on the active playlist, and "record to a new playlist" is the interface's
New Playlist before Record. No object, field or message is added: the
session file and the wire stay as they are.

### 2. A swipe is a compound of existing members

`model::comp_range(snapshot, CompRange{track, from, to, start, end,
seam_crossfade}, next_id, out)` returns the members that make `[start,
end)` of playlist `to` play what `[start, end)` of playlist `from` plays:

1. What `to` holds over the range is cleared: a region inside goes, one
   cut at one edge is trimmed and loses the fade on the cut edge, one
   spanning both is split at `end` and trimmed at `start`.
2. Each of `from`'s regions under the range is placed on `to`, cut to the
   range, with a new id, its name, colour, gain, mute and the fades whose
   edges the range did not cut.
3. Each crossfade of `from` that the range holds whole is set between the
   placed copies.
4. Seam crossfades, §3.

New ids come from `next_id` in member order, every split id taken, as
every compound's are. A gap in the lane clears the target there: a swipe
copies a range, and a range includes its silence, as Paste does. The
caller sends the members as one `Compound` ("Copy to Playlist" or the
interface's word), so a swipe is one revision and one undo step, and
refused whole if a member is. A swipe that needs more than
`kMaxCompoundMembers` members is refused before it is sent, by name.

The target need not be the active playlist: "comp to a new playlist" is
the interface's New Playlist (AddPlaylist + SetActivePlaylist) followed by
swipes, and comping into an inactive playlist while listening to another
is allowed, as Pro Tools allows copying to any playlist.

Refused: a MIDI or instrument track, a lane or target that is not a
playlist of the track, a lane equal to the target, an empty or negative
range.

### 3. Seams

`seam_crossfade == 0` leaves the joins as cuts, Pro Tools' behaviour and
the bit-exact one. A positive length asks for a centred equal-power
crossfade at each join between a placed region and the target's region it
butts. Each is fitted (`fit_crossfade`, ADR-0021) to the handles the two
regions have and to half the length of each, so the two seams of a short
piece never meet inside it, then tried against the edit as built so far;
one the model refuses is left out. The result counts the seams found and
the seams faded. The model never shortens on its own account (ADR-0009,
"Fail loudly"); this is the gesture's builder shortening, as ADR-0021
allows, and the interface says how many joins stayed hard.

The interface's default is a preference: off under the Pro Tools keymap,
on under the Logic one.

### 4. Where a comped region came from

The lanes show which piece of each take is in the comp. That is computed:
`model::comp_origin(track, region, exclude)` is the first playlist of the
track, other than `exclude`, holding a region that covers the region's span
with the same channels and the same source alignment
(`source_offset - position`). It is not stored: a stored origin would need a
format bump and go stale with the next trim, while the computed one stays
right for any edit that keeps the audio where it was, and is simply absent
after one that moves it.

### 5. Consolidate: the render is the engine's, the replacement is a member

Consolidate (`ui.comping.consolidate`) renders [start, end) of a playlist
(regions, fades, crossfades, gains) to one file, then makes it the only
region there. The render is the engine's offline writer. The edit needs no
new operation: the file enters the pool with `AddSource` (not undoable, as
every source), and `model::consolidate_range()` returns one `PlaceRegion` of
the whole range from the file's first frame, whose overwrite replaces what
was there. It is sent in a Compound with a label, so it is one undo step; no
protocol or format change. Refused when the range holds no region, or the
file is missing, shorter than the range or not the track's channel count.
Undo gives back the old regions; the rendered file stays in the pool.

### 6. Not decided here

- Logic's take folder, collapsed with the comp shown on the folder: an
  interface view over §1, `ui.comping.lanes`.
- MIDI comping: MIDI regions are windows onto shared sequences
  (ADR-0014); refused until MIDI takes need it.
- The interface half: `ui/qt`'s `edit.consolidate` handler, which builds
  the argument below from the selection and shows the events.

### 6.1 The entry point, decided 2026-11-04 (protocol 43)

The interface replica has no Document owner, no session folder and no
files, so Consolidate is a request to the engine, which does all of §5.

- **Request.** The engine command `session.consolidate`, argument
  `track=<id> start=<frame> end=<frame> [playlist=<id>] [name=<text>]`
  (`api/consolidate_argument.h` formats and parses it; the name runs to the
  end). No playlist is the track's active one. It is a `CommandRequest`,
  not a new wire message: the reply says only whether the render *started*.
  The cheap refusals come back in that reply, before any render: a
  malformed argument, an unknown track, a range the model would refuse
  (`consolidate_check()`, which asks `model::consolidate_range()` with the
  file's size), a take recording, requested or closing, and a consolidate
  already running (one at a time).
- **Where it runs.** On a worker thread started by the control thread
  (`ConsolidateJob`), never the audio thread: it touches no part of the
  session but whether a take is open, which the control thread checks
  before starting. The worker holds an immutable `Snapshot` copy and
  renders (`render_range()`); it never touches the `Document`. When it
  ends, the control thread's tick commits (`consolidate_commit()`) against
  the document *as it is then*, so an edit made during the render is
  respected: a removed track or a range the model now refuses leaves no
  change (the file is removed, or stays in `Audio Files/` out of the pool
  when the model refuses after the render, as §5 says). The commit is the
  one `Compound` labelled "Consolidate" and goes to every replica as a
  `ModelUpdate`; undo is §5's. If another session was opened meanwhile the
  file is removed and nothing is committed.
- **Progress and result.** `EventKind::Consolidate`: `insert` the track,
  `frame` the fraction in thousandths, `flags` one of `kConsolidateRunning`,
  `Done`, `Failed`, `Cancelled`, `text` the reason or, for Done, the new
  file. Running is sent at the start and then every 1 % at most (tick-paced).
  The last event *stands*: a late client is sent it, and it is replaced only
  by the next request, so a failure is a held notice the interface keeps
  until the operator dismisses it, not the 2.5 s status line.
- **Cancel.** `session.consolidate_cancel` sets the worker's cancel flag,
  polled once per block. The file is removed and nothing reaches the
  document; the event is Cancelled. A cancel that arrives after the last
  block still discards the result. Refused when nothing is running.
- **Shutdown.** The server cancels and joins the worker.
- **Not a lock on takes.** A take started *during* a render is not
  refused (the render reads playlist files; a take adds new ones). The
  render's result is still committed against the current document.

## Rationale

Building a swipe from existing members keeps the engine, the replica and
the session file unchanged, and so needs no version bump at all. Every
member is already checked, undone and replicated; a new operation would
have repeated that work for nothing the compound cannot say. The limit of
256 members is far above a swipe: a range across a few takes is a few
regions.

## Consequences

- The interface's lane view and swipe gesture (`ui.comping.lanes`) call
  `comp_range` and send one `Compound`.
- `ui/qt/src/edit_clipboard.cpp`'s `clear_range` and the model's clear in
  `comp_range` follow the same rule; a follow-up may make the interface
  call the model's.

## Alternatives rejected

- A `CopyRange` operation: one member instead of a few, but a protocol bump,
  a codec entry and a second implementation of overwrite and crossfade
  rules, for no edit a compound cannot express.
- A stored "comped from" field on regions: a format bump for a derived fact
  that edits make stale.
- Crossfading every seam whatever the preference: it changes the audio
  across the crossfade, and Pro Tools does not.
