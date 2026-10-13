# Timeline model, undo, and the playback plan

Status: design, written before the code. Nothing here is implemented. The
model lives in `common/model` (ADR-0007 §1, M8): standard C++17 only, no
platform header, no thread of its own (ADR-0008). The file format it loads
and saves is [`session-format.md`](session-format.md). The architectural
decisions are [ADR-0009](../decisions/ADR-0009-timeline-snapshots-and-plan-swap.md).

Phase 2's exit criterion is the test of this design: **record, play back,
punch in over an existing take, and undo it, without stopping the
transport.** Every section below says what it contributes to that.

## Who touches what

| Thread | Module | Touches |
|---|---|---|
| Document thread (one, non-RT) | `engine/control` | Owns the `Document`: applies operations, keeps the undo stack, asks for plans, serialises saves. The only writer of the model. |
| Plan builder (same thread, or a worker it owns) | `engine/core`, non-RT functions | Reads a snapshot, compiles a `PlaybackPlan`. |
| Disk reader (new, non-RT) | `engine/core` | Fills the source cache for the windows the plans need. |
| Disk writer (today's `DiskThread`) | `engine/core` | Drains capture rings. Never waits on the reader. |
| Audio thread | `engine/core` | Reads the current plan and the cache. Nothing else from this document. |
| UI replica | `ui/client` | Its own copy of the model, kept current by operations (below). |

**`common/model` is never on the audio thread** (ADR-0007 §2). Its types
allocate, share ownership and may hold strings. The audio thread sees only
the plan, a flat block of POD built from the model, and it never sees a
`shared_ptr`. A reference count dropping to zero on the audio thread is a
`free` on the audio thread.

## The model

Types are sketches of shape; the rt-engineer settles signatures in the M8
PR (ADR-0007 §6: `common/model`'s public headers are contract files).

```cpp
namespace sonora::model {

using Id = uint64_t;           // one counter for every object
using Frames = int64_t;        // samples at the session rate; signed for arithmetic,
                               // never negative in a valid model

struct SourceRef { Id source; uint16_t channel; };

struct Fade { Frames length = 0; FadeShape shape = FadeShape::Linear; };

struct Region {                // value type, immutable once in a playlist
    Id id;
    std::string name;
    Frames position, length, source_offset;
    std::vector<SourceRef> channels;   // one per track channel
    float gain_db = 0.0f;
    bool mute = false;
    Fade fade_in, fade_out;
};

struct Crossfade { Id left, right; Frames length; Align align; FadeShape shape; };

struct Playlist {              // immutable; shared between snapshots
    Id id;
    std::string name;
    std::vector<Region> regions;        // sorted, non-overlapping
    std::vector<Crossfade> crossfades;
};

struct TrackEdit {             // the undoable part of a track
    Id id;
    std::string name;
    uint16_t channels;
    Id active_playlist;
    std::vector<std::shared_ptr<const Playlist>> playlists;
};

struct EditState {             // one undo step's worth of state
    std::vector<std::shared_ptr<const TrackEdit>> tracks;   // display order
    std::shared_ptr<const MarkerList> markers;
};

struct Snapshot {              // what a save, a plan or a replica reads
    std::shared_ptr<const EditState> edit;
    std::shared_ptr<const SourcePool> sources;  // append-only
    MixState mix;                               // gain, pan, mute, solo, monitor bus
    SessionSettings settings;                   // rate, timecode, tempo, transport
    uint64_t revision;
};

}
```

### Snapshots, not mutation

An `EditState` is never modified. An edit builds a new one that **shares
every unchanged part** with the previous: moving a region on track 3 copies
track 3's `TrackEdit` and the one `Playlist` touched, a vector of regions,
and points at the other 17 tracks unchanged. Cost per edit is proportional
to the playlist touched, not the session.

That gives three things for the price of one:

- **Undo is a pointer.** The undo stack holds the `EditState` before and
  after each operation. Undo restores the previous pointer. There is no
  inverse operation to write, so there is no inverse operation to get wrong.
  A wrong inverse is how a DAW's undo corrupts a session, and here it is
  how a take would disappear.
- **Saves and plans read a consistent value without a lock.** The document
  thread hands a `Snapshot` to the saver and the plan builder; they read it
  while the next edit is applied to a new one.
- **Replicas can be checked.** A replica that applies the same operations
  must reach a value equal to the engine's; a test compares them.

### What undo covers

| State | Undoable | Why |
|---|---|---|
| Tracks' playlists, regions, fades, crossfades; active playlist; track names and order; markers | yes | The edit. This is `EditState`. |
| Sources | **no**, append-only | Undoing a take removes its region, never its audio. The source stays in the pool, unused, available to comping. Losing a take is the worst outcome. |
| Mix state: gain, pan, mute, solo, monitor bus | no | As in Pro Tools, a fader move is not an edit. If mix lived in `EditState`, undoing a trim would also jump the faders back. |
| Settings: timecode, tempo, loop, punch | no | Session settings, changed deliberately. |

The undo stack is bounded: 200 steps by default. When a step falls off the
end, the `EditState`s only it referenced are freed on the document thread.
*Unverified: memory per undo step on a large session.* The worst case is an
edit on a long playlist, about 100 bytes per region copied. Measurement for
the M8 PR: 18 tracks × 2,000 regions, 200 single-region edits on the longest
playlist, peak RSS. If it is over about 64 MB, playlists get chunked
(copy-on-write per 256 regions) before anything else changes.

## Operations

Every change to the document is an `Operation`: a plain value, serialisable,
and **fully resolved**. Resolved means it carries every id and every number
the result depends on, so applying it is a pure function:

```cpp
Snapshot apply(const Snapshot& before, const Operation& op);   // or an error
```

The engine resolves an operation from a command before applying it (a
`split at playhead` command becomes `SplitRegion{region 21, at 3840000,
new_right_id 57}`). Nothing in `apply` allocates an id, reads a clock or
looks at a file. That is what lets the same function run in the engine and
in a UI replica and produce the same value.

**Operations are the replication format** (ADR-0007 M8). The engine sends
each applied operation, with the revision it produced, to every replica. A
replica applies it and checks the revision is the next one. Undo and redo
are sent as `Undo{revision}` and `Redo{revision}`, and the replica moves its
own history pointer; it keeps the same bounded stack. A gap or a mismatch
makes the replica ask for a full snapshot, which is the session file's
canonical JSON. One codec serves saving, replication resync and test
fixtures.

### Operations Phase 2 needs

| Operation | Undoable | For |
|---|---|---|
| `AddSource{source}` | no (pool) | every take, every import |
| `RecordTake{track, playlist, region, punch_xfades}` | yes | recording and punch-in, below |
| `PlaceRegion{playlist, region}` | yes | import, paste. Overwrites what is beneath (below) |
| `MoveRegion`, `TrimRegion{edge, to}`, `SplitRegion{at, new_id}`, `RemoveRegion` | yes | basic editing |
| `SetFade{region, in/out, fade}`, `SetCrossfade{left, right, ...}`, `RemoveCrossfade` | yes | fades and crossfades |
| `AddPlaylist`, `SetActivePlaylist` | yes | the format carries playlists now; Phase 3 uses them |
| `AddMarker`, `MoveMarker`, `RemoveMarker` | yes | locate targets |
| `SetTrackName`, `ReorderTracks` | yes | |
| `SetMix{track, field, value}` | no | faders; also reaches the audio thread as an atomic |
| `SetSettings{...}` | no | timecode, tempo, loop, punch range |

Region moves, trims and edit modes beyond these (Shuffle, Spot, Grid,
nudge) are Phase 3 commands that resolve into the same operations.

### Overwrite: how a region goes on top of another

Playlists never overlap (session format; ADR-0009 §3). Placing a region
over `[a, b)` resolves into: every existing region fully inside is removed;
a region straddling `a` is trimmed to end at `a`; a region straddling `b`
is trimmed to start at `b`; a region spanning both is split in two, the
right part getting a new id carried in the operation. Crossfades whose
regions changed are removed or kept by the same rule. All of it happens in
one operation, so one undo restores all of it.

### Punch in, which is the exit criterion

Rolling, with track T armed and a punch range `[A, B)` set, over an existing
region R:

1. **Before A**, the plan plays R on T. If the pre-roll passes through
   `A − H`, capture starts there, with `H = max(punch_crossfade, 1 s)` of
   handle, as far as the pre-roll allows. The engine knows `A` in advance,
   so starting early costs nothing. The capture start is sample-accurate: the
   punch points travel to the audio thread as `RtCommand`s with frame
   positions (ADR-0007 §2).
2. **From A to B**, T monitors its input (the `"auto"` rule), and capture
   continues to `B + H` or the end of the pass.
3. **At punch-out**, the take closes **without stopping the transport**.
   The document thread then applies, as one undoable step: `AddSource`
   (not undoable, pool) and `RecordTake`, whose overwrite splits R into
   `R₁ = [.., A)` and `R₂ = [B, ..)` and places the new region N at `[A, B)`.
   It also adds two crossfades of `punch_crossfade` samples, centred on
   `A` (`R₁`→`N`) and on `B` (`N`→`R₂`), drawing on R's own material beyond
   the cut and on N's handle.
4. A new plan is built and swapped in (below). The next pass plays N.
5. **Undo**, still rolling, restores the `EditState` before step 3. A plan
   for it is built and swapped. The next pass plays R, whole. N's source
   stays in the pool.

The latency term: the take's source is shifted earlier on the timeline by
the round-trip latency `L` (capture plus playback), so that what was played
lines up with what was heard. In model terms N's `source_offset` is
`(A − capture_start_position) + L`. `L` comes from the loopback measurement
(Phase 2 backlog: input monitoring with latency compensation). Until that
exists it is the backend's reported latency, and the source records which
figure was used (`latency_compensation`).

What the engine does not do today and must, for this sequence:

- **Close a take while rolling.** `Session::stop()` stops the transport and
  joins the disk thread to close files. Punch-out needs a take boundary that
  is not a transport stop.
- **Fix the take-epoch race first.** `planning/BACKLOG.md`, "Known gaps": a
  callback that loaded `Recording` for the previous take can write into a
  ring being reset. Back-to-back punches are exactly the start/stop cycle,
  at the operator's speed, not after a 100 ms stall. The epoch handshake and
  its regression test are a prerequisite of punch, not a follow-up.
- **Separate "rolling" from "recording" per track.** The transport rolls;
  capture is a per-track window inside it.

## The playback plan

The audio thread reads a `PlaybackPlan`, built off-thread from a
`Snapshot`, published through an atomic pointer, retired by epoch and freed
off the audio thread. That mechanism is ADR-0007 §2. This document adds
what the plan contains and when it may be published.

### Contents

One allocation, sized when built, freed as a whole. No pointers into the
model. Trivially destructible.

```cpp
struct PlanSegment {                  // a region, flattened
    int64_t start, end;               // timeline [start, end)
    uint32_t source_slot[2];          // index into the session's source table
    uint16_t source_channel[2];
    int64_t source_offset;            // source frame at `start`
    float gain;                       // linear, region gain
    int64_t fade_in_end, fade_out_start;
    uint8_t fade_in_shape, fade_out_shape;
    // A crossfade is two segments that overlap by its length, each with
    // its fade set: the only place two segments overlap.
};

struct PlanTrack {
    uint32_t engine_track;            // index of the engine's Track
    uint32_t first_segment, segment_count;   // sorted by start
    uint32_t delay_frames;            // PDC; 0 in Phase 2, the field exists now
};

struct PlanNode {                     // the graph, in topological order
    NodeKind kind;                    // TrackOut, BusSum, HardwareOut
    uint32_t inputs_begin, inputs_count;
    uint32_t scratch_buffer;          // preallocated at session creation
};

struct PlaybackPlan {
    uint64_t epoch;
    uint64_t model_revision;          // which snapshot it came from
    PlanTrack* tracks;  uint32_t track_count;
    PlanSegment* segments;
    PlanNode* nodes;    uint32_t node_count;
    uint32_t monitor_out[2];          // physical playback channels
    float pan_law;                    // per-track coefficients use the atomics
};
```

The audio thread finds the segment under the playhead by keeping a cursor
per track, advanced as it plays and reset by binary search after a locate
or a swap. Never a scan from the start.

### The monitor bus plan

Phase 2's graph is fixed: one `TrackOut` node per track, one `BusSum`, one
`HardwareOut` with two channels. Per block, for each track:

1. Choose input or playback by the track's monitor mode and armed state.
2. Playback: render the segments under `[playhead, playhead + nframes)`
   from the source cache, with region gain and fades.
3. Apply the track's fader and mute, and pan into L and R with the session's
   pan law. **These are atomics on `Track`**, read once per block, as gain
   and mute already are. A fader move is not a plan swap.
4. Sum into the bus. Apply bus gain. Write to the two output ports.

Solo is computed from the atomics too: any solo set means unsoloed tracks
contribute nothing. It is a per-block check, not a structural change.

It is a node list rather than a hard-coded loop for the reason the owner
gave: inserts, sends, VCA and groups arrive on top of the same graph. Phase
2 executes it on the audio thread in order. Phase 4's parallel scheduler
executes the same list with dependencies (`plugin-latency-compensation.md`),
and the `delay_frames` per path is where PDC goes. Neither needs the plan's
shape to change, only its node kinds.

### When a plan may be published: residency first

This is the part that makes "undo without stopping the transport" true
rather than nearly true. A new plan may name material that is not in memory:
undo brings back R, whose middle the cache dropped when N replaced it.
Swapping in a plan whose audio is on the disk gives silence until the reader
catches up, a hole in the playback, right where the operator is listening.

So:

- **Playback reads through a source cache, not through per-track rings.**
  The disk reader loads fixed-size chunks of source files (64 k frames,
  *unverified*, see below) into a pool of buffers allocated at session
  creation. The audio thread looks a chunk up by `(source_slot,
  chunk_index)` in a fixed-size lock-free table. If a chunk is not there,
  the audio thread plays silence for it and counts an underrun, the same
  rule as a capture overrun: count it, never block.
- **A plan is published only once the chunks it needs are resident:** from
  the current playhead to one read-ahead window beyond, plus the loop start
  when looping. The document thread builds the plan, the reader loads its
  window, and only then is the pointer swapped. Until then the old plan keeps
  playing, correctly, for the edit as it was. The delay is a disk read, tens
  of milliseconds, invisible against an operator's reaction time.
- **The same gate applies to locate.** The reader loads the target window,
  then the locate `RtCommand` goes to the audio thread. "Locate without
  dropping a block" (Phase 2 backlog) is this rule.
- **Chunk reclamation uses the plan's epoch rule.** The reader unlinks a
  chunk from the table, then reuses its buffer only after the audio thread
  has started a later block. That is the same proof as retiring a plan.
  When the backend is deactivated, the reader reuses buffers at once.

Why a chunk cache and not the per-track playback ring Ardour uses: a
per-track ring holds audio *already rendered for the old plan*. After undo
it is wrong for as many seconds as it is deep, and it has to be overwritten
in place, ahead of a reader that is consuming it, which is a race in the
very path the exit criterion exercises. Chunks hold source audio, which an
edit does not change, so a plan swap invalidates nothing already loaded.
The cost: fades, crossfades and region gain run on the audio thread. That
is a multiply-add per sample per voice, at most two voices per track.

Sizing: the pool is fixed at session creation (CLAUDE.md: buffers are sized
when the session is created) and locked with the rest of the engine's
memory. Two windows per playing track, one for the current plan and one for
the plan being prepared. At 8 s per window, 18 tracks, 48 kHz, float: about
55 MB. *Unverified: the window size.*

### Recording and playback share the disk

The disk writer drains capture; the disk reader fills the cache. They are
separate threads so that a slow read never delays a capture drain, since a
late drain is an overrun and an overrun loses part of a take. When both
want the device, the writer wins.

*Unverified, and it sets the read-ahead window and the chunk size:*
sustained mixed throughput on the persistence partition of a real USB
stick, 18 tracks recording at 24-bit/48 kHz while 18 play back, chunk sizes
of 64 k and 256 k frames; worst-case read latency is the figure that
matters, not the average. 18 in plus 18 out at 24-bit/48 kHz is about
5.2 MB/s of mixed I/O; ROADMAP Phase 6 already calls 4.6 MB/s of writes
alone (32 tracks) the wrong load for a live medium, so this is close to the
edge on a stick. This goes to the researcher before the
reader is written, and the number goes in `brain/research/`.

## What the UI replica gets

`ui/client` holds a `Snapshot` and applies the engine's operations to it
(ADR-0007 §3, M7). It never applies an operation of its own to its replica:
it sends a command, the engine resolves and applies it, and the replica
changes when the operation comes back. One writer, so there is nothing to
reconcile. A UI that wants to show a drag in progress draws it as interface
state, outside the model, until the engine's operation arrives.

## Platform neutrality

`common/model` uses standard C++17 only (ADR-0008): no file system calls,
no threads, no clocks. It turns bytes into a `Snapshot` and back, and it
applies operations. Opening files, `fsync`, scanning `Audio Files/` and
reading BWF headers are its callers' business: `engine/control` through
`common/platform` and `common/audiofile`. That keeps `apply` pure, which is
what replication and the tests both rely on.

## Tests the M8 PR starts with

- Load every fixture, save, and compare bytes: canonical output.
- `apply` then undo returns an `EditState` equal to the original, for every
  operation, including overwrite in each of its four cases.
- A replica fed the operation log equals the engine's snapshot.
- Each invariant of the session format is rejected at load with its key
  path.
- `RecordTake` over a region, then undo: the source is still in the pool.
- In `engine/core`, against `drivers/fake`: punch in, punch out and undo
  while rolling, and assert no output block is silent that should not be,
  and that the captured file's samples land at the compensated position.
  That is the Phase 2 exit criterion as a ctest.

## Open questions

- Chunk size and read-ahead window: the researcher's USB measurement above.
- Undo memory on a large session: the M8 measurement above.
- Whether the plan builder runs on the document thread or a worker. It is
  the document thread unless building a plan for 18 tracks of 2,000 regions
  is measured above about 20 ms.
- Phase 3's loop recording creates a take per pass. Whether each pass is a
  `RecordTake` on an alternate playlist, or one source with pass markers, is
  Phase 3's decision. Both fit this model.
