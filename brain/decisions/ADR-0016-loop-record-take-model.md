# ADR-0016 — Loop record: one file per take, one region per pass, passes as alternate playlists

- Status: **Proposed**, 2026-10-25. Not implemented. The owner decides
  O1–O3 ("For the owner"); each has a recommended default.
- Date: 2026-10-25
- Proposed by: rt-engineer (Track 1), from BACKLOG Track 1 item 4
  ("Remain: loop, pre-roll and input monitoring") and the open question in
  `brain/specs/timeline-model.md` ("Phase 3's loop recording creates a take
  per pass ... Phase 3's decision").
- Amends, if accepted: nothing. It adds one model operation, one
  `CaptureBoundary` kind and one bext field.
- Numbering: follows ADR-0015.

## Summary

*Navigation only; the sections below are the decision.*

- §1: one loop record is one take file per track, written straight through
  the wraps; each pass is a region of it.
- §2: the audio thread marks each wrap in band with a `Wrap` boundary
  record on the existing ring; no file opens or closes at a wrap.
- §3: the last complete pass goes on the active playlist; every pass also
  gets its own new playlist, in pass order, as Pro Tools' loop record does
  with alternates. One resolved operation per track, one undo step.
- §4: the bext Description says `loop=<A>-<B> passes=<n>`.

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
   (one undo step). `kMaxCompoundMembers` is 256. An `AddPlaylist` and a
   `RecordTake` per pass per track is 2·N·T members: 18 tracks hit the
   limit at the eighth pass.
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

### 2. Wraps in band

`CaptureBoundary` gets `Kind::Wrap = 3`: `at` is the capture count of the
first sample after the wrap, `timeline` is A. The audio thread pushes one
per armed track at the block offset where the playhead wraps, after the
samples before it, as it does Open and Close; a full ring is an overrun,
counted, and that take is marked failed rather than placed with passes in
the wrong place. Nothing new is allocated: the ring and its capacity are
the session's. The disk thread does not split files at a Wrap; it hands
the list of wrap counts to `engine/control` with the `ClosedFile`, in a
vector sized at session creation (the pass cap, O2).

Loop playback (`engine.transport.loop`) uses the same split: the block is
rendered as [offset 0, wrap) at the old position and [wrap, end) from A.
The residency rule already keeps chunks at the loop start resident
(ADR-0009 §4.3).

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
does. One member per track keeps a Compound at T members, 18 for the
UMC1820, whatever the pass count. The active playlist stays active.

`apply` is pure and the passes are part of the resolved operation, so
replicas and undo work as they do for `RecordTake` (ADR-0009 §2). It is a
new operation on the wire: the protocol version is the next free number at
landing.

### 4. bext

The Description gets `loop=<A>-<B>` in place of `punch=`, written when the
file opens, so a take killed mid-loop says it was one. Recovery
(`engine.record.orphan_offer`) offers an orphan loop take as one region
over its whole length, since the wrap list was not on disk; that is the
same loss as a killed punch's crossfades.

## For the owner

- **O1, which pass is on the active playlist.** Recommended: the last
  complete pass, as Pro Tools leaves it. A pass cut short by stop is still
  kept on its own playlist, never on the active one. Alternative: the last
  pass however short.
- **O2, the pass cap.** Recommended: 99 passes per loop record; at the
  99th wrap the take keeps recording but later wraps are counted, not
  listed, and a Notice says so at stop. It sizes the wrap list at session
  creation.
- **O3, a playlist per pass or a take list.** Recommended: a playlist per
  pass, so passes appear as alternates in the playlist lanes
  (`ui.comping.lanes`) with no new document concept. Alternative: one
  take-list per track (Pro Tools' clip list with matching start times),
  which needs a new model type and a format bump.

## Consequences

- Loop record writes no more files than a plain take; the disk thread's
  work at a wrap is one integer.
- A loop record of N passes on T tracks adds N·T playlists. A session of
  many loop records grows its playlist list; deleting unused playlists is
  a comping command (`ui.comping.consolidate`).
- Pre-roll runs only before the first pass; post-roll does not apply while
  looping.

## Alternatives rejected

- **One file per pass** (a Close and an Open at each wrap): no handle at B
  for any pass, so no loop-end crossfade, and a file per pass per track.
- **`AddPlaylist` + `RecordTake` per pass**: the Compound limit stops 18
  tracks at seven passes, and raising the limit grows every replica's
  worst case.
- **Pass markers on one region, no playlists**: the comping UI would need
  a second way to show alternates; the format already has playlists for
  this.
