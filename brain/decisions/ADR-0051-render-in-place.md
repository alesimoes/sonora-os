# ADR-0051 — Render in place: Track Freeze, Commit and Process Selection

- Status: Accepted (recommended defaults, taken by the implementer with no
  owner available, 2026-11-04)
- Date: 2026-11-04
- Progress items: `plugins.graph.freeze` (Pro Tools: Track Freeze, Track
  Commit; Logic Pro: Bounce in Place), `plugins.offline` (Pro Tools:
  AudioSuite; Logic Pro: selection-based processing)
- Depends on: ADR-0004 (plug-ins out of process), ADR-0012 (hosting, any
  miss fails a render), PR #378 (the bounce path this follows)

## Summary

*Navigation only; the sections below are the decision.*

- §1: one engine path prints a track through its own live plug-in chain to
  a Float32 file in `Audio Files/`, then puts the file in the track as one
  Compound. Three uses: Freeze (reversible), Commit (kept), Process
  Selection (a range, on a copy of the playlist).
- §2: the render runs in real time, on the live chain, through a
  `TrackTap` on the chain's output. It is not offline, and that is the
  boundary, not a shortcut: the hosts are other people's code in processes
  of their own and run only at the audio callback.
- §3: sample-accurate: the tap reports the chain's latency, the worker drops
  that many frames and the lead-in, the file lands where the live track
  played.
- §4: it fails rather than limps: a block the chain did not play, a miss, a
  full ring, a latency change, a disk miss, an edit or a bypass moved during
  the render each leave no file.
- §5: the document: a freeze's playlist carries a `freeze` record (session
  format 23); Unfreeze is its inverse; Commit and Process leave plain
  playlists. Protocol 56.
- §6: what this is not, and what is open.

## Context

Offline bounce (PR #378) renders a private `Session` on a worker, and refuses
a session with an active insert: "bounce in real time instead". The same is
true of a track. `plugin-latency-compensation.md` lists freeze as the escape
hatch for a track too expensive for real time and says it "should exist
before the scheduler is pushed to its limit".

## Decision

### 1. One path, three uses

`RenderInPlace` (engine/control) takes a `RenderRequest`: the track, the
range `[start, end)` and a tail. `model::render_range_of()` decides the range
(the extent of the active playlist for Freeze and Commit, the selection for
Process) and the refusals; `model::render_members()` builds the Compound
that places the file.

| Use | Playlist after | Inserts | Undone by |
|---|---|---|---|
| Freeze | new, with `freeze{from, inserts}` | the active ones made inactive | Unfreeze, or Undo |
| Commit | new, plain | made inactive | Undo |
| Process | a copy of the active playlist with the range replaced | made inactive | Undo |

Inactive inserts are not instantiated and have no latency (`Insert::active`),
which is what a freeze is for: the DSP is gone, the plan loses the chain's
delay line. Process makes them inactive too, because their effect is now in
the audio and keeping them live would apply it twice; the workflow is "set
the plug-in up live, select, Process". The chain that renders is the chain
as the document has it: a bypassed insert is not in the print.

A track already frozen refuses all three ("unfreeze it first"). Unfreeze
returns to `freeze.from`, reactivates the inserts still on the track and
takes the frozen playlist off; the rendered file stays in the pool.

### 2. Real time, on the live chain

The only way to run a hosted plug-in is the audio callback, so the live
session plays the range with the transport and its audio thread pushes the
chain node's output into a `TrackTap` (`engine/core/track_tap.h`, a
`RingBuffer` of interleaved L/R, pre-fader). A worker drains it to disk.
Nothing allocates, locks or waits on the audio thread: a chunk that does not
fit is dropped and counted. The cost is wall-clock time (a 4-minute track
takes 4 minutes) and that the transport is occupied; the benefit is that the
audio is exactly what the operator heard, and no second host is started.

An offline render would need a second set of hosts driven from a worker
without a callback; ADR-0012's boundary is built around the callback, and
building a second one for this is a decision of its own (open, §6).

### 3. Alignment

The chain's output is `latency_frames` late (ADR-0012 §7). The tap carries
that latency with every block (`node.latency_frames`); the worker drops
`lead + latency` frames, where `lead` is the pre-roll (1 s, clamped at the
session start) rolled to warm dynamics and discarded. The file's first frame
is the timeline `start`, written as its BWF `TimeReference`. The engine test
renders through a runner that delays by 300 frames and requires the file to
equal the source to the sample, for a render from 0 and for a selection that
starts 12000 frames in.

The tail (default 2 s for Freeze and Commit, 0 for Process) is how long the
roll goes on past the audio, for a reverb's ring-out.

### 4. Failing clearly

ADR-0012 §3: "During a bounce or freeze, any miss fails the render. A file
with a hole is never produced." The render is refused, and the file removed,
when:

- the chain was not bound, silent, restarting or run in pieces for any block
  of the range (`TrackTap::unplayed_frames`); dry audio is never written, it
  would be early and unprocessed;
- the chain's host missed a block (`PluginHosting::chain_misses`: slot
  misses, non-finite blocks, silent blocks, read when the first frames
  arrive and again at the end);
- the ring had no room (`lost_frames`), the disk did not serve a block
  (`playback_misses`), or the chain's latency changed;
- the transport stopped before the end of the range (the operator's Stop is
  a cancel);
- the track's active playlist or active inserts, or an insert's bypass,
  changed while it rendered (`RenderBasis`): the print no longer matches the
  track.

A start is refused while a take is open, while the transport is not stopped,
while a bounce or another render runs, and when the chain is not running
(`PluginHosting::chain_ready`): the operator is told to wait for the plug-ins
to load. The end of a render, and every refusal, is a held notice in the
footer (`EventKind::Render`), not the 2.5 s line.

### 5. Document, wire, file

- `Playlist::freeze` (`from`, `inserts`) and the optional `freeze` key of
  `AddPlaylist`. The model refuses a freeze of a playlist or an insert the
  track does not have. `DuplicatePlaylist` and `DuplicateTrack` do not copy
  it: a copy is a plain playlist.
- Session format 23: a playlist may carry `"freeze": {"from": <id>,
  "inserts": [<id>...]}`. A version 22 file has none and opens as it was; a
  file that says 22 and carries the key is refused. The migration is the
  reader's check.
- Protocol 56: commands `track.freeze`, `track.commit`,
  `track.process_selection` (argument `track=<id> [start= end=] [tail=]
  [name=]`, `api/render.h`), `track.unfreeze`, `track.render_cancel`;
  capability `track.render`; `EventKind::Render` (`frame` thousandths,
  `insert` the track, `flags` a `RenderState`, `text`). The command reply
  says only whether the render started.
- The Compound is labelled "Freeze Track", "Commit Track", "Process
  Selection", "Unfreeze Track" in the undo history.
- UI: Track menu entries (the selected track, and for Process its selected
  range) and a footer line with progress, Cancel and the held end.

### 6. Not this, and open

- **Offline (faster than real time) rendering through hosted plug-ins.** Not
  built; it needs a second driver for the hosts (§2). Native stock
  processors (`plugins/native`) could be rendered in process in an offline
  loop, but ADR-0011 §1.4 gates promoting them into the engine, so not here.
- Process Selection applies the whole chain of the track. Applying one
  plug-in of several means bypassing the others first.
- Commit of a frozen track is Unfreeze then Commit; there is no operation
  that clears a freeze record in place.
- Editing a frozen playlist is not refused; Unfreeze then discards the
  edits with the playlist (the audio files stay in the pool).
- Region edits, fades and automation of the original playlist are printed
  (the tap is after the chain, fed by playback); the track's fader, pan,
  sends and mute are not (pre-fader).
- Unverified: a real out-of-process host through this path. The tests use a
  runner with a declared latency, as the PDC tests do; the first hardware
  run should render a track with a look-ahead limiter and compare the file
  with the live output by ear and by null.

## Consequences

- Freeze releases the DSP of every frozen track at the cost of a render.
- The audio-thread addition is one branch on an atomic pointer per chain
  node per callback and one `RingBuffer` write for the one tapped track.
- A render occupies the transport for its length.
