# ADR-0016 — Loop record: one file per take, one region per pass, passes as alternate playlists

- Status: **Accepted**, 2026-10-25, by the coordinator under the owner's standing authorization of 2026-10-21 (recommended defaults O1–O3, architect changes C1–C7). Not implemented. Revised the same day
  with the architect's assessment of PR #114 ("accept with changes",
  #issuecomment-6394446768, C1–C7). The owner decides O1–O3 ("For the
  owner"); each has a recommended default, which the architect holds.
- Date: 2026-10-25
- Proposed by: rt-engineer (Track 1), from BACKLOG Track 1 item 4
  ("Remain: loop, pre-roll and input monitoring") and the open question in
  `brain/specs/timeline-model.md` ("Phase 3's loop recording creates a take
  per pass ... Phase 3's decision").
- Amends, if accepted: nothing. It adds one model operation, one
  `CaptureBoundary` kind, a wrap sidecar per loop take and one bext field.
- Numbering: follows ADR-0015.

## Summary

*Navigation only; the sections below are the decision.*

- §1: one loop record is one take file per track, written straight through
  the wraps; each pass is a region of it. Loop plus punch is refused at
  start; the loop range is frozen while recording; a loop shorter than the
  handle or one block is refused.
- §2: the audio thread marks each wrap in band with a `Wrap` boundary
  record on the existing ring; no file opens or closes at a wrap. The disk
  thread persists each wrap with the take's checkpoint.
- §3: the last complete pass goes on the active playlist; every pass also
  gets its own new playlist, in pass order, as Pro Tools' loop record does
  with "Automatically Create New Playlists When Loop Recording". One
  resolved operation per track, 2·T compound members, one undo step.
- §4: the bext Description says `loop=<A>-<B>`.
- §5: recovery and a full boundary ring both still place every pass;
  nothing recorded is ever left off the timeline.

## Context

The engine records while rolling with in-band take boundaries, punches at
any frame inside a block (PR #81), places the take over the punch range
with crossfades that draw on handle material (PR #91), and follows the
punch out (#101). Loop playback (`engine.transport.loop`) and loop record
are the next transport items. The session already stores a loop range
(`Transport::loop`, `[0, 0]` unset), and a track already has several
playlists and an `active_playlist` (`session-format.md`), "so the Phase 3
change is behaviour, not format".

What has no decision is the take model: what a pass becomes in the
document. The timeline spec names two candidates, a `RecordTake` per pass
on an alternate playlist, or one source with pass markers. Three facts
decide between them:

1. **Crossfades need handles.** `RecordTake` refuses a crossfade with no
   handle material, and does not shorten it. A take written as one file
   per pass has no audio past B on any pass, so a pass on the active
   playlist could never crossfade out at the loop end. In one continuous
   file, the samples after pass *n*'s B are what the player played next,
   pass *n*+1's start, which is the handle a loop-end crossfade wants.
   Pass *n*'s start has pass *n*−1's end before it the same way, and the
   first pass has the pre-roll.
2. **The compound limit.** A take is placed as one `Compound` per stop
   (one undo step), each file as `AddSource` plus `RecordTake`
   (`add_closed_take`, `files_per_compound = kMaxCompoundMembers / 2`).
   `kMaxCompoundMembers` is 256. An `AddSource`, then an `AddPlaylist` and
   a `RecordTake` per pass, per track, is T·(1 + 2·N) members: 18 tracks
   hit the limit at the eighth pass.
3. **The audio thread.** A wrap lands inside a block. Closing and opening
   files at it would put a Close and an Open on the boundary ring per track
   per pass and a file create on the disk thread per pass. Nothing about it
   is unsafe, but it buys nothing that point 1 does not take away.

## Decision

### 1. One take file per track per loop record

Record with a loop range set and loop on records from the start of the
pre-roll (or A) until stop, straight through every wrap, into one file per
armed track, as a take does today. The file's TimeReference is the
timeline frame of its first sample, as now. Pass *k* is the file's samples
between wrap *k*−1 and wrap *k*; its region is placed at A with the
source offset of that span, length B − A.

**Starting inside the loop.** If record starts with the playhead anywhere
other than A − pre-roll, the transport locates to A − pre-roll first, as
Pro Tools does; pass 1 is always a whole pass.

**Loop and punch together are refused at start.** `transport.record` and
`transport.punch` with loop on and a punch range set (the #91
`document_punch` path) are refused before anything is armed, with a
Notice naming both ranges in the engine's "frame N" spelling.
`follow_punch_out` (#101) never applies to a loop take. Logic's cycle plus
autopunch is a later item. Pro Tools keeps loop record and QuickPunch as
separate record modes.

**The loop range is frozen for the take.** While a loop take records, a
`SetSettings` that moves `Transport::loop` is refused, by name, and
turning loop off is refused the same way: stop ends the take. This holds
the wraps to the A and B the bext Description gives (§4), which recovery
depends on (§5).

**A short loop is refused at start.** B − A must be at least the pass
handle H and at least one block (the session's maximum block size), so at
most one wrap lands in any callback and §2's sizing holds.

### 2. Wraps in band, persisted with the checkpoint

`CaptureBoundary` gets `Kind::Wrap = 3`: `at` is the capture count of the
first sample after the wrap, `timeline` is A. The audio thread pushes one
per armed track at the block offset where the playhead wraps, after the
samples before it, as it does Open and Close. Nothing new is allocated:
the ring and its capacity are the session's. A full ring is an overrun:
counted, never waited on, and handled as §5 says.

`at` is the count that lines up with A once the input latency L is
applied. That is the same `source_offset` term the punch path defers to
`engine.playback.input_monitor` (`timeline-model.md`, "Punch in, which is
the exit criterion", the latency term), and it is deferred the same way:
until the loopback measurement lands, L is the backend's reported latency
here as it is for a punch, and the source records which figure was used.

> Note 2026-10-25: superseded by ADR-0019. L is the loopback measurement
> (`--measure-latency`) or `--input-latency`, 0 when neither is given, not
> the backend's reported latency.

The disk thread does not split files at a Wrap. It appends the wrap's file
frame to a sidecar `<take file>.wraps`: a fixed header (magic, version,
A, B) and one 8-byte little-endian record per wrap, append-only. The
sidecar is synced with the take's existing `fdatasync` checkpoint, so the
loss window stated for a take's samples covers its wraps too. This is
disk-thread work and may block; the audio thread is not involved. At stop
the list goes to `engine/control` with the `ClosedFile`, in a vector sized
at session creation to the pass cap (O2). Every wrap is written to the
sidecar, including those past the cap.

Loop playback splits the block at the wrap; its details belong to
`engine.transport.loop`.

### 3. Placement: `RecordLoopTake`, one per track

A new resolved operation in `common/model`:

    struct RecordLoopTake {
        Id track = 0;
        Id active_playlist = 0;     // receives the chosen pass
        std::vector<LoopPass> passes;  // pass order; each: playlist id,
                                       // playlist name, region
        std::size_t chosen = 0;     // index into passes
        Id split_right_id = 0;
        std::vector<Crossfade> crossfades;  // on the active playlist only
    };

`apply` adds one new playlist per pass holding only that pass's region,
named `<track name>.T<take>.P<k>`, and places the chosen pass on the
active playlist with the punch crossfades at A and B, as `RecordTake`
does. The active playlist stays active.

Each file is still placed as `AddSource` plus its one `RecordLoopTake`, so
a loop take is 2·T members in one Compound, 36 for the UMC1820, whatever
the pass count. Past 128 tracks it splits across compounds exactly as
`RecordTake` does (`files_per_compound`).

**A crossfade without a handle is left out, never fatal.** If stop lands
exactly on a wrap, or pre-roll is 0, pass 1 at A or the last complete pass
at B has no handle. The controller leaves that crossfade out of the
operation and the stop Notice names it, as #91 does for a crossfade the
model refuses. A missing handle never refuses the operation.

`apply` is pure and the passes are part of the resolved operation, so
replicas and undo work as they do for `RecordTake` (ADR-0009 §2). It is a
new operation on the wire: the protocol version is the next free number at
landing.

### 4. bext

The Description gets `loop=<A>-<B>` in place of `punch=`, written when the
file opens, so a take killed mid-loop says it was one. The pass count is
not in it: it is unknown at open, and the sidecar (§2) records it.

### 5. Every pass is placed: recovery and a full ring

- **Recovery** (`engine.record.orphan_offer`) offers a recovered loop take
  as its passes on new playlists, the same as a normal stop, never as one
  region. It reads the wraps from the sidecar. If the sidecar is missing
  or stops early, it derives the rest from the bext `loop=<A>-<B>`, the
  TimeReference and L: the first wrap falls at file frame
  B − TimeReference (+ L), each later one B − A after the last, and the
  offer says the passes were placed by loop length. That is exact only if
  the take had no capture overrun: `session.cpp` drops samples on a short
  capture-ring write, which shifts everything after it. The loop range is
  frozen (§1), so A and B in the Description are the take's.
- **A full boundary ring** still places the take. The controller splits
  the file by the same arithmetic as recovery and posts a Notice. If even
  that is impossible (no `loop=` A and B known, which §1 rules out), the
  whole file goes on its own new playlist as one region. Nothing recorded
  is ever left unplaced.

## For the owner

- **O1, which pass is on the active playlist.** Recommended: the last
  complete pass, as Pro Tools leaves it. A pass cut short by stop is still
  kept on its own playlist, never on the active one. Alternative: the last
  pass however short.
- **O2, the pass cap.** Recommended: 99 passes per loop record; at the
  99th wrap the take keeps recording. Passes past the cap stay in the
  file, the stop Notice gives their count, and their wraps are saved in
  the sidecar (§2), so they remain recoverable though not placed. The cap
  sizes the wrap list at session creation.
- **O3, a playlist per pass or a take list.** Recommended: a playlist per
  pass, so passes appear as alternates in the playlist lanes
  (`ui.comping.lanes`) with no new document concept. Alternative: one
  take-list per track (Pro Tools' clip list with matching start times),
  which needs a new model type and a format bump.

## Consequences

- Loop record writes one file and one small sidecar per track; the disk
  thread's work at a wrap is an 8-byte append.
- A loop record of N passes on T tracks adds N·T playlists. A session of
  many loop records grows its playlist list; deleting unused playlists is
  a comping command (`ui.comping.consolidate`).
- Pre-roll runs only before the first pass; post-roll does not apply while
  looping.

## Unverified

1. **The playlist count's cost on undo and resync.** The worst case is 99
   passes × 18 tracks = 1,782 new playlists in one step. Measure its
   bytes against the ADR-0009 §1 byte-bounded undo stack and its size in
   the resync window (`encode_window()`), and the time a replica takes to
   apply it. Researcher; not blocking.

## Update, 2026-10-25: loop playback when not recording

§2 left loop playback to `engine.transport.loop`. As built (rt-engineer,
Track 1; it changes no take, file or protocol):

- **The range is the document's.** As for loop record, a set loop range
  is loop on: `PlaybackController::show()` passes it to
  `Session::set_loop()`, and a roll, not recording, wraps from B to A at
  the sample inside a block, with the loop take's split in
  `render_track()`. A block that starts at or past B plays on. A range
  shorter than one block is not looped (as §1 refuses it for a take).
  A plain or punched take never wraps: a take is one contiguous span.
- **The range reaches the audio thread behind the residency gate**
  (ADR-0009 §4.4). `set_loop()` asks the reader to hold [A, A + W), cut
  at B; a new `RtCommand::Kind::Loop`, in order with the locates, goes
  only once that generation is resident (`pump_loop()`). A roll or take
  requested meanwhile waits for it.
- **The old range stays whole until the new one is taken.** The reader
  holds both starts, and runs the playhead's window to the later B,
  until `Transport::loops_applied()` reaches the new range's count; so a
  range moved while rolling neither drops the old start nor misses what
  lies past the old B.
- **A loop shorter than W** is [A, B) entire for both windows: read once,
  nothing past B read for it.
- **A loop take** holds its own range in the reader; at stop or punch-out
  the playback range is held again.
- Still open: `transport.toggle_loop`. With the range as the on switch,
  a toggle that keeps the range needs a loop-enabled flag in the session
  settings (Pro Tools' Loop Playback and Logic's Cycle are modes apart
  from the range); that is a format change and waits for its own decision.
  Decided 2026-10-26, below.

## Update, 2026-10-26: the loop on switch

Taken on the recommended defaults (`transport.toggle_loop`).

- **`transport.loop_playback`, saved, outside undo** (session format 9,
  protocol 17): Pro Tools' Loop Playback toggle, which Pro Tools saves with
  the session and starts off. A new session has it off.
- **It gates both loops.** A roll wraps, and a take is a loop take (§1),
  only with the flag on and a range set. With it off the range is kept and
  a take records straight through it, as Logic's does with Cycle off; one
  switch, so a lit LOOP never means "playback only". `document_loop()` and
  `PlaybackController::show()` read it; the audio thread is unchanged
  (`Session::set_loop(0, 0)` as for no range).
- **The command edits the document.** `transport.toggle_loop` is handled
  by the control server (the recovery commands' pattern): a `SetSettings`
  with the flag flipped, sent to every replica before the reply. Turned on
  with an argument, the timeline selection as `A-B` (what `transport.play`
  takes), the selection becomes the range, as Pro Tools loops the
  selection; turned off, the range stays. Refused while loop record runs,
  as a range change is.
- **Migration v8 to v9:** a file with a loop range opens with the flag on,
  so it plays as it did; one without opens with it off.
- "It gates both loops" is superseded for takes by the next update: loop
  record has its own mode.

## Update, 2026-10-26: loop record as a record mode

Taken on the recommended default (`transport.toggle_loop_record`, the
follow-ups to #144's review).

- **A per-session setting, not a per-profile behaviour.** The engine never
  knows which profile the interface runs, and a session must record the
  same whichever profile opens it, so the difference lives in the document
  and in which command each profile's key sends. `transport.loop_record`,
  saved and outside undo beside `loop_playback` (session format 10,
  protocol 23), off in a new session: Pro Tools' Loop Record (Options >
  Loop Record, Option+L), a record mode apart from Loop Playback.
- **Loop record alone makes a loop take.** `document_loop()` reads
  `loop_record` and a range; `loop_playback` gates only the roll's wrap
  (`PlaybackController::show()`). So in Pro Tools, Loop Playback on to
  audition a section and Record gives a straight take through it; Loop
  Record on gives a loop take, with Loop Playback on or off. C3 (loop and
  punch refused together) applies only with loop record on.
- **Three commands, one handler.** `transport.toggle_loop` flips loop
  playback, `transport.toggle_loop_record` flips loop record; each turned
  on with the selection as its `A-B` argument makes it the range, as Pro
  Tools loops the selection. `transport.toggle_cycle`, Logic's C, sets both
  modes together to the opposite of loop playback and never moves the
  range: Logic's Cycle is one switch for both and keeps the cycle area.
  The Qt router attaches the selection to the first two only.
- **Frozen while loop record runs.** A loop take refuses any change to the
  range, to loop playback or to loop record, from the commands or from a
  `SetSettings` (`loop_take_refusal()`), so no screen shows a mode the
  take is not in.
- **The Transport window** shows Loop and Loop Rec in a profile that binds
  no `transport.toggle_cycle`, and one Cycle button in one that does.
- **Migration v9 to v10:** loop record opens as loop playback was, since a
  version 9 take was a loop take exactly when loop playback was on.
- Not done here: in Pro Tools the loop follows the Edit selection while
  Loop Playback is on; here the range moves only when a mode turns on.
  Recorded in the backlog under `engine.transport.loop`.
- Unverified: whether Pro Tools turns Loop Playback off when Loop Record is
  turned on (they are kept independent here), and Option+L's quotation in
  Avid's guide (bound `"verified": false`).

## Update, 2026-10-26: Logic's cycle area

Taken on the recommended default (#149 review, blocking item 1). With C
bound to `transport.toggle_cycle` and the selection never sent, the Logic
profile had no way to set a range: in a new session C turned Cycle on over
`[0, 0]` and R recorded a straight take.

- **C takes the selection when no area is set.** `transport.toggle_cycle`
  takes the selection as an optional `A-B` argument, which the Qt router
  attaches as it does for the other two. Turning Cycle on with no range set
  (`loop[1] <= loop[0]`) makes the selection the range; a range already set
  stays, as Logic's C keeps the cycle area. With neither, the command is
  refused ("no cycle area: select a range first") and no mode turns on: a
  Cycle button lit over nothing would arm a take that is not a loop take.
- **Cmd+U, `transport.cycle_selection`.** Logic's Set Locators by Selection
  and Enable Cycle: the selection, required, becomes the range and both
  modes turn on; with Cycle already on it moves the range. No existing
  command fits, since every other one toggles. Refused with no selection,
  and like the others while loop record runs. Bound `"verified": false`.
- Inside protocol 23, which this change's own PR introduced (19 and 21 were never used).

## Update, 2026-10-26: transport settings at the authority

Taken on the recommended default (#156 review, N1 and N2). An interface
edited the punch range and the rolls by sending `SetSettings` with a whole
copy of the settings from its replica. That copy can be stale: it reverts
any field the engine changed since (a mode toggled by a key on another
interface, a loop range set by Cmd+U), and a punch edit that arrives during
a take, through a recording overlay not yet updated, moves the saved range
away from the one the take uses.

- **One command per field, run by the engine.** `transport.punch_selection`
  (the selection as `A-B`, required; the Qt router attaches it as it does
  for the loop commands), `transport.clear_punch`, `transport.set_pre_roll`
  and `transport.set_post_roll` (frames, as `transport.locate` reads a
  frame). Each handler edits its one field on the engine's own document
  and broadcasts the update, as the loop commands already do. The
  interface sends only what changed, so no stale copy crosses the wire.
  Punch is refused with no selection and with loop record on (C3), as the
  Qt stand-in did.
- **The punch range is frozen while a take is open or requested.** A change
  is refused by name, from these commands and from a `SetSettings`
  (`take_punch_refusal()`, beside `loop_take_refusal()`), from record until
  the take stops: the take's Description names its range, which recovery
  reads back. A take that is closing has the range in its files already, so
  it does not hold it. The rolls are in no file and a take copies them when
  it starts, so they still change during a take, for the next roll (the
  "any other setting still applies" of the loop take, test_control).
- **The rule for every `SetSettings` sender.** A setting the operator edits
  from an interface while the engine or another interface may change
  settings gets a field command at the engine; a whole-settings
  `SetSettings` is only for the authority itself (the engine's handlers,
  the recovery offer), tests, and a document not yet sent. One sender is
  left: the interface's rename after an engine Save (`session_model.cpp`),
  which copies the name and id into its snapshot; it moves to the engine's
  Save handler, recorded in the backlog.
- **Rejected: compare-and-set on a base revision.** A `ModelRequest`
  carrying the revision its edit was built on, refused when settings
  changed after it. The interface pipelines edits optimistically, so its
  base for a second edit includes a first still in flight; a refused first
  edit makes every later base wrong, and an unrelated edit would need a
  separate settings revision kept by every engine and replica. Larger, and
  it refuses an edit the operator meant rather than applying the one field.
- **Rejected: a field mask on `SetSettings`.** It would change the op
  codec and every replica's decoder for what four commands do.
- Protocol 25 (24 is held for the fades PR in flight). Session format
  unchanged.

## Alternatives rejected

- **One file per pass** (a Close and an Open at each wrap): no handle at B
  for any pass, so no loop-end crossfade, and a file per pass per track.
- **`AddPlaylist` + `RecordTake` per pass**: the Compound limit stops 18
  tracks at seven passes, and raising the limit grows every replica's
  worst case.
- **Pass markers on one region, no playlists**: the comping UI would need
  a second way to show alternates; the format already has playlists for
  this.
- **Wraps only in memory until stop**: a crash loses every pass's place,
  the take-loss case the product ranks worst.
