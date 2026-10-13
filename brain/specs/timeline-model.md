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
| Document thread (one, non-RT) | `engine/control` | Owns the `Document`: applies operations, keeps the undo stack, builds and publishes plans, retires them. The only writer of the model. |
| Plan builder (the document thread, or a worker it owns if measured too slow) | `engine/control` (ADR-0007 §2) | Reads a snapshot, compiles a `PlaybackPlan` whose layout `engine/core` defines. |
| Saver (one, non-RT) | `engine/control` | Every save, autosave and backup, in order, through `common/platform`. |
| Disk reader (new, non-RT) | `engine/core` | Fills the plans' chunk slots for the residency window; unlinks and reclaims chunks. |
| Disk writer (today's `DiskThread`) | `engine/core` | Drains capture rings, splits takes at boundary records, `fdatasync`s periodically. Never waits on the reader. |
| Audio thread | `engine/core` | Reads the current plan, its chunk slots and the `Track` atomics. Nothing else from this document. |
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

- **Undo is a pointer.** The undo stack holds one `EditState` per step: step
  *i* is the state after operation *i*, step 0 the state at open. Undo moves
  the pointer back one step, redo forward. There is no
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

The undo stack is **bounded by bytes, not steps** (ADR-0009 §1). Each step
is charged the bytes it alone allocated: the playlists and tracks it copied,
not those it shares with its neighbours. While the total is over the budget
(64 MiB by default, a session setting), the oldest step is evicted and the
values only it referenced are freed on the document thread. The newest step
is never evicted, so even a bulk edit larger than the budget can be undone
once. A step count would bound nothing: a nudge copies one playlist, a
select-all nudge copies all of them.

The engine alone decides eviction. Every history message to a replica
(operation, `Undo`, `Redo`) carries the **undo floor**, the oldest revision
still undoable, and the replica evicts down to it. Both sides then offer the
same steps, whatever their allocators count.

*Unverified: memory per undo step on a large session.* The worst case is an
edit on a long playlist, about 100 bytes per region copied. Measurement
before M8: 18 tracks × 2,000 regions, 200 single-region edits on the longest
playlist plus bulk edits, peak RSS. If a few dozen steps exceed the budget,
playlists get chunked (copy-on-write per 256 regions) before anything else
changes.

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
are sent as `Undo{revision, floor}` and `Redo{revision, floor}`, and the
replica moves its own history pointer; each operation also carries `floor`,
and the replica evicts history below it (above). A gap or a mismatch
makes the replica ask for a full snapshot: the document with its whole undo
window (`encode_window()`, ADR-0009's 2026-10-13 update), so the replica
offers the same undo and redo as the engine from then on. A session file
would not do: it holds one state. In `contracts/api` that payload is
**opaque bytes**: `contracts/api` cannot depend on `common/model` (ADR-0007
§1), so the engine encodes and `ui/client` decodes with `common/model`.

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
   so starting early costs nothing. The punch points travel to the audio
   thread as `RtCommand`s with frame positions (ADR-0007 §2), and both the
   capture start and stop are **sample-accurate inside a block**: the audio
   thread computes the frame offset of `A − H` within the block and copies
   into the capture ring from that offset, not from the block's start.
2. **At A**, T's monitoring switches from playback to input (the `"auto"`
   rule) at the exact frame inside the block, as a ramp of `punch_crossfade`
   samples centred on A with the crossfade's shape. That is the same
   crossfade the take will get in step 3, so what the performer hears while
   punching is what plays back afterwards. At **B** the same ramp switches
   back. Capture continues to `B + H` or the end of the pass.
3. **At punch-out**, the take closes **without stopping the transport**, by
   an in-band boundary (below). Once the disk thread reports the take closed,
   the document thread applies, as one undoable step: `AddSource`
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

### Closing a take while rolling: in-band boundaries

`Session::stop()` today stops the transport, joins the disk thread to close
files, and resets the capture rings before the next take. Punch-out needs a
take boundary that is not a transport stop, and back-to-back punches would
run that reset at operator speed, which is the take-epoch race in
`planning/BACKLOG.md` ("Known gaps"). The boundary therefore travels **in
band, with the audio**:

- Per track, the audio thread keeps a running count of frames it has pushed
  into the capture ring. It pushes frames only inside a capture window.
- At a window's start and end it pushes a **boundary record** on an SPSC
  ring to the disk thread: `{track, take, kind (open|close), at_frame}`,
  where `at_frame` is that running count, plus, for `open`, the timeline
  position of the first frame. The record is pushed after the frames it
  refers to (release), so the disk thread never sees a boundary ahead of its
  audio.
- The disk thread drains the capture ring up to the next boundary's
  `at_frame` and **splits at exactly that frame**: the frames before go to
  the closing take's file, whose header it finalises and closes; the frames
  after go to the next take's file.
- **The capture ring is never reset.** There is nothing to race against, so
  the take-epoch handshake is no longer needed. This replaces it; the
  regression test for the old race is still written first, and must pass.
- The disk thread **opens the next take's files ahead**, as soon as a take
  starts, so opening a file is never in the drain path at a boundary.
- After closing a take, the disk thread reports **"take closed, length N"**
  to `engine/control`. `AddSource` and `RecordTake` are applied only then,
  with N from disk, not computed from the transport.
- "Rolling" and "recording" are separate per track: the transport rolls,
  and capture is a per-track window inside it.

A boundary record ring that is full is an overrun of the same kind as a
full capture ring: counted, never a wait. Its capacity (at least four
records per track) is fixed at session creation.

## The playback plan

The audio thread reads a `PlaybackPlan`, built by `engine/control` from a
`Snapshot`, published through an atomic pointer, retired by epoch and freed
off the audio thread. That mechanism is ADR-0007 §2; the exact retirement
protocol, memory orders included, is ADR-0009 §4.1 and is not restated
here. This document adds what the plan contains and when it may be
published.

### Contents

One allocation, sized when built, written in full by the builder before it
is published (so every page is faulted in off the audio thread and locked
by M2's `mlockall(MCL_FUTURE)`), freed as a whole. No pointers into the
model. Trivially destructible. Its structure is immutable; the chunk-slot
array is its one mutable part, written only by the disk reader.

```cpp
struct PlanSegment {                  // a region, flattened
    int64_t start, end;               // timeline [start, end)
    uint32_t source_slot[2];          // index into the session's source table
    uint16_t source_channel[2];
    int64_t source_offset;            // source frame at `start`
    uint32_t first_chunk_slot[2];     // per channel, into chunk_slots
    uint32_t first_chunk_index[2];    // source chunk number of that first slot
    float gain;                       // linear, region gain
    int64_t fade_in_end, fade_out_start;
    uint8_t fade_in_shape, fade_out_shape;
    // A crossfade is two segments that overlap by its length, each with
    // its fade set: the only place two segments overlap.
};

struct PlanTrack {
    uint32_t engine_track;            // index of the engine's Track
    uint32_t first_segment, segment_count;   // sorted by start
};

struct PlanNode {                     // the graph, in topological order
    NodeKind kind;                    // TrackOut, BusSum, HardwareOut
    uint32_t stable_id;               // survives plan swaps; keys delay-line state
    uint32_t first_input_edge, input_edge_count;
    uint32_t scratch_buffer;          // preallocated at session creation
};

struct PlanEdge {                     // from's output feeds to's input
    uint32_t from_node, to_node;
    uint32_t delay_frames;            // PDC; 0 in Phase 2, the field exists now
};

struct PlaybackPlan {
    uint64_t epoch;                   // how the audio thread detects a new plan
    uint64_t model_revision;          // which snapshot it came from
    PlanTrack* tracks;  uint32_t track_count;
    PlanSegment* segments;
    PlanNode* nodes;    uint32_t node_count;
    PlanEdge* edges;    uint32_t edge_count;   // grouped by to_node
    std::atomic<uint32_t>* chunk_slots; uint32_t chunk_slot_count;
    uint64_t peak_chunk_demand;       // max over window positions, for the pool check
    uint32_t monitor_out[2];          // indices into AudioBlock::outputs
    float pan_law;                    // per-track coefficients use the atomics
};
```

- **Delay sits on edges**, because PDC delays a path into a node, not the
  node. The **delay-line state lives outside the plan**, in `engine/core`,
  keyed by `(to_node.stable_id, edge)`, preallocated to the session's maximum
  delay. A swap that keeps a node keeps its delay line's contents, so a plan
  swap never restarts a delay from silence.
- **Fixed maxima at session creation:** tracks, nodes, edges, scratch
  buffers, source slots, chunk slots per plan, maximum delay. The builder
  refuses a plan that exceeds any of them and names the limit.
- **Chunk slots replace a lookup table.** A segment's channel *c* plays
  source chunk `k` from slot `first_chunk_slot[c] + (k −
  first_chunk_index[c])`: an index computation and one acquire load. The
  slot holds a pool buffer index or "empty".
- The audio thread detects a new plan by `epoch`, never by pointer
  (ADR-0009 §4.1: a freed plan's address can be reused).

The audio thread finds the segment under the playhead by keeping a cursor
per track, advanced as it plays and reset by binary search after a locate
or a new epoch. Never a scan from the start.

### The monitor bus plan

Phase 2's graph is fixed: one `TrackOut` node per track, one `BusSum`, one
`HardwareOut` with two channels. Per block, for each track:

1. Choose input or playback by the track's monitor mode and armed state;
   a switch that falls inside the block (punch A or B) happens at its frame,
   as a ramp (see "Punch in").
2. Playback: render the segments under `[playhead, playhead + nframes)`
   from the chunk slots, with region gain and fades.
3. Apply the track's fader and mute, and pan into L and R with the session's
   pan law. **These are atomics on `Track`**, read once per block, as gain
   and mute already are. A fader move is not a plan swap. Each is **ramped
   linearly across the block** from the value applied at the end of the
   previous block, so a fader jump or a mute is never a step.
4. Sum into the bus. Apply bus gain, ramped the same way.
5. `HardwareOut` writes L and R into `AudioBlock::outputs[monitor_out[0]]`
   and `outputs[monitor_out[1]]`, and writes silence into every other output
   it was given. The M2 backend contract (`contract/backend`, PR pending)
   already carries `outputs` and `output_count` and requires every frame of
   every output to be written, because a backend buffer holds whatever the
   last cycle left in it.

Solo is computed from the atomics too: any solo set means unsoloed tracks
contribute nothing, ramped to and from zero like mute. It is a per-block
check, not a structural change.

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

- **Playback reads through source chunks, not through per-track rings.**
  The disk reader loads fixed-size chunks of source audio, converted to
  float (8 k to 16 k frames, *unverified*, see below), into a pool of
  buffers allocated at session creation, and links each into the slots of
  the plans that play it. It **coalesces** adjacent chunks of one source into
  a single read. If a slot is empty when the audio thread needs it, the
  audio thread plays silence for that span and counts an underrun, the same
  rule as a capture overrun: count it, never block.
- **A plan is published only once the chunks it needs are resident:** from
  the current playhead through the residency window, plus the loop start
  when looping. The document thread builds the plan, the reader fills its
  window, and only then is the pointer exchanged. Until then the old plan
  keeps playing, correctly, for the edit as it was.
- **Only the newest plan waits.** If a newer plan is built while one is
  pending, the pending plan is cancelled with its queued reads and freed at
  once (it was never published). Plans are never queued.
- **The residency window is derived from the measurement, not chosen:**
  `W` = the playhead's advance during the worst-case coalesced read latency,
  times 2, plus one block. The worst case, not the average, is the figure.
- **The same gate applies to locate.** The reader loads the minimum window
  at the target, then the locate `RtCommand` goes to the audio thread.
  "Locate without dropping a block" (Phase 2 backlog) is this rule. Locate
  latency is bounded by one coalesced read per source channel in that
  window; the engine measures every locate and publishes p99 and max in
  diagnostics, and past 100 ms the UI shows "Locating…". It never starts
  playing a window that is not resident.
- **Chunk reclamation uses the plan's retirement rule** (ADR-0009 §4.1).
  The reader stores "empty" into every slot naming a chunk, reads the block
  counter, and reuses the buffer only once the counter has moved. When the
  callback is stalled nothing is reused; after `deactivate()` returns,
  everything unlinked is reused at once.
- **Pool exhaustion never evicts the playing plan.** Eviction order: chunks
  linked by no plan; then the playing plan's chunks behind the playhead and
  outside the loop-start window. Never the playing plan's chunks in
  `[playhead, playhead + W)` or at the loop start. If the pending plan still
  does not fit, it stays pending, the UI shows "edit not yet audible:
  playback cache full" with a counter, and it is published at the next
  stop. A plan whose `peak_chunk_demand` exceeds half the pool is flagged
  when built, before it plays.

Why a chunk cache and not the per-track playback ring Ardour uses: a
per-track ring holds audio *already rendered for the old plan*. After undo
it is wrong for as many seconds as it is deep, and it has to be overwritten
in place, ahead of a reader that is consuming it, which is a race in the
very path the exit criterion exercises. Chunks hold source audio, which an
edit does not change, so a plan swap invalidates nothing already loaded.
The cost: fades, crossfades and region gain run on the audio thread. That
is a multiply-add per sample per voice, at most two voices per track.

Sizing: the pool is a fixed number of chunks, set at session creation
(CLAUDE.md: buffers are sized when the session is created), pre-faulted by
writing every page once on a non-RT thread, and locked with the rest of the
engine's memory. Demand is counted per source channel and per region in the
window, not per track: a track needs, summed over the segments overlapping
the window, *channels* × (⌈overlap / *C*⌉ + 1) chunks. So a stereo track
counts twice, a crossfade counts both its sources, and a comped playlist
costs at least one chunk per channel per region in the window, however
short. Two plans (playing and pending) may need disjoint chunks. Example: 18
stereo tracks, 16 k-frame chunks (64 KiB as float), a 4 s window, one region
per window: 2 × 13 × 18 × 2 = 936 chunks, about 59 MiB. *Unverified: chunk
size, window, and the demand of a 2,000-region comp.*

### Recording and playback share the disk

The disk writer drains capture; the disk reader fills the cache. They are
separate threads so that a slow read never delays a capture drain, since a
late drain is an overrun and an overrun loses part of a take. When both
want the device, the writer wins.

*Unverified, and it sets the read-ahead window and the chunk size:*
sustained mixed throughput on the persistence partition of a real USB
stick, 18 tracks recording at 24-bit/48 kHz while 18 play back, chunk sizes
of 8 k and 16 k frames with coalesced reads; worst-case read latency is the figure that
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
applies operations. It returns bytes; it never writes them. Opening files,
`fsync`, scanning `Audio Files/` and reading BWF headers are its callers'
business: `engine/control` through `common/platform` and
`common/audiofile`. That keeps `apply` pure, which is what replication and
the tests both rely on. It parses through `common/json`, which is not part
of `common/base` because `common/base` must stay RT-safe.

## Tests the M8 PR starts with

- Load every fixture, save, and compare bytes: canonical output.
- `apply` then undo returns an `EditState` equal to the original, for every
  operation, including overwrite in each of its four cases.
- A replica fed the operation log, floors included, equals the engine's
  snapshot and offers the same undo steps.
- Undo eviction: past the byte budget the oldest steps go and the newest
  never does.
- Each invariant of the session format is rejected at load with its key
  path.
- `RecordTake` over a region, then undo: the source is still in the pool.

And in `engine/core`, with the reader and the punch work:

- The ThreadSanitizer stress test of the retirement protocol (ADR-0009
  §4.1): a fake callback thread, a publishing document thread and a reader
  linking and unlinking chunks, at least 10⁶ swaps, with deactivate and
  reactivate cycles. In CI.
- Boundary records: a take split at exactly `at_frame` for frame offsets 0,
  1, `nframes − 1` inside a block; back-to-back punches with no gap; the
  old take-epoch race's regression test passing with a stalled callback.
- Against `drivers/fake`: punch in, punch out and undo while rolling, and
  assert no output block is silent that should not be, and that the
  captured file's samples land at the compensated position. That is the
  Phase 2 exit criterion as a ctest. **It depends on ADR-0007 M3**: it needs
  `drivers/fake` with outputs captured, so it cannot land before M3.

## Open questions

- Chunk size and residency window: the researcher's USB measurement above.
- Undo memory on a large session, and pool demand of a 2,000-region comp:
  measured before M8 (`planning/BACKLOG.md`, Phase 2).
- Whether the plan builder runs on the document thread or a worker that
  `engine/control` owns. It is the document thread unless building a plan
  for 18 tracks of 2,000 regions is measured above about 20 ms.
- Phase 3's loop recording creates a take per pass. Whether each pass is a
  `RecordTake` on an alternate playlist, or one source with pass markers, is
  Phase 3's decision. Both fit this model; the boundary records already
  split a pass at a frame.
