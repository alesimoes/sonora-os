# ADR-0164 - Clip launcher: a quantised state machine fed by an SPSC command queue

## Context

`logic.live_loops` adds a clip launcher: a grid of tracks x scenes where each
cell starts or stops a clip, quantised to the beat or the bar. The arrangement
timeline (`common/model`) is the session's document; it is edited off the
audio thread, persisted by the disk side and read by the playback path. A
launcher reacts to performance gestures at any moment, and its starts and
stops must fall on the exact frame of a musical boundary whatever the audio
block size. The audio thread may not allocate, lock or throw (CLAUDE.md).

## Decision

1. The launcher is a quantised state machine in its own module,
   `common/launch`, separate from the arrangement timeline. Standard C++17,
   no module edges, no Qt, no platform header (ADR-0007, ADR-0008).
   - `launch_grid.h`: `LaunchGrid` of tracks x scenes, sized at
     construction and never resized; a `Cell` holds a `ClipId` and a state
     (`Stopped`, `Queued`, `Playing`, `QueuedStop`). A clip is a reference
     to an existing region by id; the launcher never owns audio. One cell at
     most plays per track column.
   - `launch_quantize.h`: `Quantize` (`Immediate`, `Beat`, `Bar`, `Bars2`,
     `Bars4`), a `TransportSnapshot` (sample rate, tempo, beats per bar,
     anchor frame of bar 1) and `next_boundary(pos, snapshot, q)`, the first
     boundary at or after `pos`. Boundary k is `anchor + round(k * step)`,
     computed from k alone, so it never drifts.
   - `launcher.h`: the abstract `Launcher` (`assign_clip`, `set_quantize`,
     `trigger_cell`, `trigger_scene`, `stop_track`, `stop_all`;
     `set_transport` and `advance(block_start, frames)`). `advance` returns a
     fixed-capacity `TransitionList` of the starts and stops inside the
     block, each with its frame offset.
   - `state_machine_launcher.h`: the reference `StateMachineLauncher`. Each
     column holds the playing scene and one pending action (start a scene or
     stop) with its frame; a newer command on the column replaces it. A
     re-trigger in the column stops the playing cell and starts the new one
     on the same boundary frame. A trigger on an empty cell stops the column;
     a stop with nothing playing cancels a queued start with no transition.
     A column yields at most one stop and one start per block, so the list
     is sized 2 x tracks at construction and cannot overflow.
2. Audio-thread input is a single-producer single-consumer queue of
   fixed-size, trivially copyable commands, its storage allocated at
   construction. The control methods only push; a full queue refuses the
   push and counts an overrun, nothing waits. `advance` drains at most the
   queue's capacity, quantises each command from `block_start`, then fires
   every pending action whose frame is in `[block_start, block_start +
   frames)`; a frame already behind the block (the transport jumped) fires
   at offset 0. No allocation, lock or throw; a test replaces the global
   allocator and counts zero allocations across 8000 `advance` calls.
3. Launched takes may later be recorded into the arrangement: the
   transitions carry the clip, track and absolute frame needed to write
   regions afterwards, off the audio thread. That is a follow-up, not part
   of this contract.

## Rationale

A state machine with one pending action per column is the whole behaviour of
a launcher: queue, land on the boundary, replace. Keeping it out of the
arrangement model means the audio thread never edits a document that the disk
and UI threads also read, and the model's heavy headers stay out of the
callback. Computing boundaries from an index rather than by adding steps keeps
them frame-exact at fractional frames-per-beat (44.1 kHz at 123 BPM). Returning
transitions with offsets lets the playback path split its block at the exact
frame instead of rounding to the block.

## Consequences

- The playback path reads a `TransitionList` per block and starts or stops
  region playback at each offset; wiring it into `Session::process()` is the
  engine slice of `logic.live_loops`.
- The grid is the audio thread's. The UI learns cell states from the
  transitions forwarded through a ring (to be designed with the API slice),
  not by reading `grid()`.
- Clip assignment goes through the same queue as triggers, so it is ordered
  with them and never races the audio thread.
- The module joins the build through a guarded hook in
  `common/pitch/CMakeLists.txt` while the root file is held; it moves to the
  root when that frees. Its tests carry the `launch` label.

## Alternatives rejected

- Launching by mutating the arrangement model live: inserting a region
  allocates, takes the model's locks and races the disk thread that reads the
  same document; it also turns every performance gesture into an undo step.
- Per-clip timers (a clock per cell counting down to its start): each timer
  rounds independently and drifts against the bar line, and a timer that ends
  mid-block is not sample-exact unless it is reduced to a frame anyway.
- `common/base`'s `RingBuffer` for the commands: it would add a module edge
  for a queue of whole structs that is twenty lines here.

## Unverified

- How the transport and tempo snapshot is read from the audio callback (who
  produces `TransportSnapshot` each block, the tempo map slice under a tempo
  change, the anchor after a locate or loop wrap). Handed to the researcher.
- Behaviour while the transport is stopped: whether `advance` is called with
  a free-running frame clock or not at all.
- Per-clip quantise and launch modes (gate, toggle, legato) beyond one global
  quantise; follow-ups.
- RTSan has not been run on `advance`; the no-allocation claim rests on the
  counting-allocator test and code review.
