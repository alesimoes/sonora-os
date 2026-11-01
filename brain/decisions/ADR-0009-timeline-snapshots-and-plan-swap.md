# ADR-0009 — Timeline as immutable snapshots, operations as undo and replication, residency-gated plan swap

- Status: Accepted (engine owner review 2026-10-13, changes applied). The
  2026-10-14 update (compound operations) was accepted by the owner,
  2026-10-18.
- Date: 2026-10-12; revised 2026-10-13 after the engine owner's review
- Decided by: architect. Rests on one owner decision, 2026-10-12: Phase 2
  playback starts with a fixed 2-channel monitor bus, and the full mixer
  topology (inserts, sends, VCA, groups) comes later on top of the same graph.
  Reviewed by the engine owner, 2026-10-13: accepted with changes, all
  applied in this text.
- Specs: [`session-format.md`](../specs/session-format.md),
  [`timeline-model.md`](../specs/timeline-model.md).

## Summary

*Navigation only; the sections below are the decision. `grep -n '^#' <this
file>` gives each heading's line; Read from there with offset/limit.*

- §1: the edit state (`EditState`) is an immutable value sharing unchanged
  tracks and playlists; one state per step, undo and redo move a pointer,
  and the stack is bounded by bytes, not steps.
- §2: every change is a resolved `Operation`; `apply(Snapshot, Operation)`
  is pure. The engine's document thread applies and replicates each one;
  replicas never apply their own and resync on a revision gap.
- §3: sources are an append-only pool; mix state is saved but not undoable;
  a playlist's regions never overlap.
- §4: `engine/control` builds the plan, `engine/core` runs it. 4.1 one
  `seq_cst` counter retires plans; 4.2 at most one plan pending; 4.3 a source
  chunk cache sized in chunks; 4.4 publication and locate wait for
  residency; 4.5 the graph in the plan; 4.6 output through
  `AudioBlock::outputs`.
- §5: the session file is canonical strict JSON (`session-format.md`); I/O
  stays out of the model; saves are durable and serialised.
- Updates: a resync carries the whole undo window (`encode_window()`); the
  server's `model::Document` is the one document and its control thread the
  document thread; `Compound{ops, label}` makes several edits one revision
  and one undo step (accepted 2026-10-18), its limit 256 at
  `kProtocolVersion` 5. Update 2026-11-01: Pro Tools' record-ready is
  engine state (`kTransportRecordReady`, layout 6, protocol 37), and Play
  starts the take.

Sections: Context; Decision §1 to §5; Rationale; Consequences; Unverified;
Updates 2026-10-13 (hardware measurements move after the Complete DAW; a
resync carries the undo window; playback on the M4/M7 control server);
Update 2026-10-14 (compound operations); Note 2026-10-18 (the compound
limit is 256); Alternatives rejected.

## Context

Phase 2's exit criterion is: record, play back, punch in over an existing
take, and undo it, **without stopping the transport**. ADR-0007 and ADR-0008
already fix the frame:

- `common/model` holds the session document and is never on the audio thread
  (ADR-0007 §1, §2).
- Structural changes reach the audio thread as an immutable plan that
  `engine/control` builds, published through an atomic pointer and freed off
  the audio thread by epoch (ADR-0007 §2).
- Undo operations double as the replication format for the UI replica
  (ADR-0007 M8).
- ADR-0008 (portability, on `main` since #9) makes `common/model` and
  `common/json` platform-neutral: standard C++17 only. File durability
  (`fsync`, `fdatasync`) lives in `common/platform`, and nowhere inline in
  `engine/core` or `engine/control` (ADR-0008 §3, seam 1).
- The backend contract on `contract/backend` (ADR-0007 M2, PR pending)
  already gives `ProcessCallback::process()` an `AudioBlock` with `outputs`
  and `output_count`, and requires the callback to write every frame of every
  output, silence included. M2 also locks memory with `mlockall(MCL_FUTURE)`
  before the session exists.

What those leave open, and what this ADR decides, are the choices that
cost a rewrite if they turn out wrong: how undo is represented and bounded,
which state it covers, whether regions may overlap, how a plan and the audio
it reads are retired, when a plan may be published given that its audio may
not be in memory, and the on-disk encoding.

## Decision

### 1. The edit state is an immutable value; undo is a pointer

The undoable part of the session (`EditState`: tracks, playlists, regions,
fades, crossfades, markers) is never mutated. An edit produces a new
`EditState` that shares every unchanged playlist and track with the previous
one.

- **One `EditState` per step.** Step *i* holds the state after operation
  *i*; step 0 is the state at open. Undo and redo move a pointer along
  the steps. Nothing stores a "before" and an "after" twice.
- **The stack is bounded by bytes, not steps.** Each step is charged the
  bytes it alone allocated (the playlists and tracks it copied, not the
  ones it shares). While the total is over the budget (64 MiB by default,
  a session setting), the oldest step is evicted and its exclusive values
  are freed on the document thread. The newest step is never evicted, even
  when it alone exceeds the budget: a bulk edit can always be undone once.
  A count limit would let 200 bulk edits on a 2,000-region playlist exhaust
  memory; a byte limit cannot.
- **The engine decides eviction, and says so.** The replica's allocator
  does not count bytes the way the engine's does, so it must not decide on
  its own. Every message that changes history (an operation, `Undo`,
  `Redo`) carries the **undo floor**: the oldest revision still undoable.
  The replica evicts down to that floor and no further. Engine and replica
  therefore offer exactly the same undo steps.

### 2. Changes are resolved operations, applied by a pure function

Every change is an `Operation` value carrying every id and number its result
depends on. `apply(Snapshot, Operation) -> Snapshot` reads no clock, allocates
no id and touches no file. The engine resolves commands into operations on
one document thread in `engine/control`, applies them, and sends each, with
the revision it produced and the undo floor, to every replica. Replicas apply
the same function and never apply their own. Undo and redo are sent as
messages that move the replica's own pointer. On a revision gap the replica
resyncs from a full snapshot. In `contracts/api` that snapshot is **opaque
bytes** (the session file's canonical JSON): `contracts/api` may not depend
on `common/model` (ADR-0007 §1), so `ui/client` decodes the bytes with
`common/model`.

This is ADR-0007 M8's "undo operations are the replication format", made
concrete: what replicates is the forward operation; undo needs no inverse
operation at all.

### 3. Sources and mix state are outside undo, and playlists do not overlap

- **Sources are an append-only pool.** Undoing a take removes its region,
  never its source or its file. Removing unused audio is an explicit,
  confirmed operator action.
- **Mix state** (gain, pan, mute, solo, the monitor bus) and session settings
  are saved but not undoable. At run time the mix values are the `Track`
  atomics they already are, applied on the audio thread with a per-block
  ramp (§4).
- **A playlist's regions never overlap.** Placing a region over others trims
  and splits them in the same operation. A crossfade is an explicit object
  between two butting regions, using each one's handle. At most two regions
  play on a track at any instant.

### 4. Plans and chunks: retirement, publication, residency

Refines ADR-0007 §2 without changing it. **`engine/control` builds the plan**,
as ADR-0007 §2 says; `engine/core` defines the plan's layout and executes it.
If plan building is measured too slow for the document thread (Unverified 4),
it moves to a worker thread that `engine/control` owns, not into
`engine/core`. ADR-0007 is not amended.

**4.1 Retirement protocol.** One counter, `blocks_` (`std::atomic<uint64_t>`,
written only by the audio thread), and the plan pointer `plan_`. All four
operations below are `seq_cst`; that is what the argument rests on.

- Audio thread, at the start of every block, in this order:
  `blocks_.store(n + 1)`, then `p = plan_.load()`. A new plan is detected
  by `p->epoch != seen_epoch_`, **never by comparing pointers**: a retired
  plan's memory can be reused for the next plan at the same address (ABA),
  and a pointer comparison would then miss the swap and keep stale cursors.
- Document thread, to publish: `old = plan_.exchange(next)`, then
  `s = blocks_.load()`, then put `{old, s}` on the retire list. `old` is
  freed once `blocks_.load() != s`.
- Why it is enough: a block that loaded `old` stored its own count *k*
  before that load, and the load precedes the exchange in the single total
  order, so *k* ≤ *s*. Once `blocks_` has moved past *s*, every block that
  could hold `old` has ended, and every later block loads `plan_` after the
  exchange.
- **Chunks follow the same rule.** The reader thread unlinks a chunk (stores
  "empty" into every slot that names it, below), then reads `s =
  blocks_.load()`, and returns the buffer to the free list once
  `blocks_ != s`.
- **The retire list is a fixed-capacity array** (8 plans), allocated at
  session creation. When it is full, the document thread stops publishing
  until an entry frees. It never frees early and never grows the list.
  Edits still apply to the document; only what is heard waits.
- **A stalled callback** (an xrun gap, freewheel, or a deactivated backend)
  does not advance `blocks_`, so nothing is freed while it is stalled. After
  `AudioBackend::deactivate()` returns, the contract guarantees no
  `process()` is running or will start, and only then are all retired plans
  and unlinked chunks freed at once. Nothing is freed on the strength of a
  timeout.
- **Required test:** a ThreadSanitizer stress test in `engine/core`'s ctest,
  with a fake callback thread running blocks while the document thread
  publishes and retires plans and a reader links and unlinks chunks, at
  least 10⁶ swaps, plus deactivate/reactivate cycles. It runs in CI.

**4.2 Publish rate.** At most **one** plan may be pending, waiting for its
audio to become resident: the newest one. When a newer plan is built, the
pending one is cancelled, with its queued reads. It was never published, so
it is freed at once, without the epoch wait. Plans are never queued: a burst
of fifty nudges costs one residency wait, not fifty.

**4.3 Source chunk cache, sized in chunks.**

- Playback reads through fixed-size **chunks** of source audio, converted to
  float, of 8 k to 16 k frames (placeholder 16 k; Unverified 1 settles it).
  The reader **coalesces** adjacent chunks of the same source into one read,
  so small chunks do not mean small I/O.
- **No lock-free hash table.** Each plan carries a **chunk-slot array**: for
  every segment and channel, one `std::atomic<uint32_t>` per chunk of source
  it can play, holding a buffer index or "empty". The reader fills slots
  (data written, then the index stored). The audio thread loads the slot
  and reads. A slot found empty is silence and a counted underrun, never a
  wait. The plan's structure is immutable; its slot array is its one
  mutable part, written only by the reader. A chunk shared by the playing
  and the pending plan is linked from both, and the reader alone keeps its
  link count.
- **Slot stores and loads are `seq_cst`, not release and acquire.**
  Amended 2026-10-13 after the RT audit of PR #31; the first text said
  release and acquire, which is enough to publish a filled chunk but not to
  reclaim one. Reclaiming is a store-buffering pattern across two
  variables: the audio thread stores `blocks_` then loads a slot; the
  reader stores "empty" into the slot then loads `blocks_` (§4.1). The
  proof needs at least one of the two to see the other's store: either the
  block loads "empty", or the reader reads a `blocks_` that already counts
  the block, and waits for it to end. Release and acquire do not order a
  store before a later load of a different variable, so both sides could
  read the old values: the block takes the buffer and the reader reuses it
  under it. Only `seq_cst` on all four accesses puts them in one total
  order. The cost is one full fence per slot store on the reader, which is
  not real-time, and nothing extra for the audio thread's loads on x86 and
  AArch64. Do not weaken these to release and acquire.
- **Demand is counted per source channel and per region**, not per track.
  For a window of *W* frames and chunk size *C*, a track's demand is the sum
  over segments overlapping the window of *channels* × (⌈overlap / *C*⌉ + 1).
  A stereo track counts twice, from one stereo source or two mono ones. A
  crossfade counts both its sources. A comped playlist costs at least one
  chunk per channel for each region in the window, however short the region
  is. The builder computes each plan's peak demand over every window
  position and reports it with the plan.
- **The pool** is a fixed number of chunks, allocated at session creation,
  pre-faulted (every page written once, on a non-RT thread) and locked.
  M2's `mlockall(MCL_FUTURE)` locks it; pre-faulting makes sure no page is
  first touched by the audio thread. Plan memory is pre-faulted the same
  way: the builder writes every byte of the plan's single allocation before
  publishing it.
- **Pool exhaustion never evicts the playing plan.** The reader evicts, in
  order: chunks linked by no plan; then chunks of the playing plan already
  behind the playhead and outside the loop-start window. Chunks the playing
  plan needs in `[playhead, playhead + W)` and at the loop start are never
  evicted. If the pending plan still does not fit, it stays pending: the old
  edit keeps playing, the UI shows "edit not yet audible: playback cache
  full" with a counter, and the plan is published at the next stop. A plan
  whose peak demand exceeds half the pool is flagged when it is built,
  before it plays.

**4.4 Publication and locate are gated on residency.**

- `engine/control` builds the plan. The **disk reader thread** (`engine/core`,
  non-RT) loads the chunks it needs from the current playhead through the
  residency window, and only then is the plan exchanged. Until then the
  previous plan keeps playing.
- The **residency window** is derived, not guessed: `W` = playhead advance
  during the worst-case read latency for a coalesced read, times a safety
  factor of 2, plus one block. The worst-case latency comes from Unverified 1.
- **Locate** uses the same gate: load the minimum window at the target, then
  send the locate `RtCommand`. Locate latency is therefore bounded by one
  coalesced read per source channel in the window. The engine measures it
  per locate and publishes p99 and max in diagnostics; past 100 ms the UI
  shows "Locating…" instead of nothing. It never starts playing a window
  that is not resident.
- The disk reader and the capture writer are separate threads. A slow read
  never delays a capture drain. When both want the device, the writer wins.
- **While stopped, a plan is published without the gate** (implementation
  note, 2026-10-13, PR #31). Nothing is heard while the transport is
  stopped, so a new plan is exchanged as soon as the reader has adopted it,
  and the gate moves to whatever starts the transport: play, record and
  locate each wait for the window at the playhead (and at a locate target)
  to be resident in the plan that will play, and for every locate asked
  before them to be applied. That is equivalent to gating the publication
  and avoids holding an edit back while nothing plays. It holds only
  because every path that starts the transport goes through that gate: a
  record started before its window is resident is the hole this section
  exists to prevent, and the first version of PR #31 had exactly that bug
  (a take right after another, or after `--place`). A take also drops any
  locate still waiting when it starts: a take is one contiguous span of the
  timeline, and its origin is the frame its first sample was recorded at,
  read on the audio thread.

**4.5 The graph in the plan.**

- Nodes in topological order (`TrackOut` → `BusSum` → `HardwareOut` in
  Phase 2), plus an explicit **edge array**: each edge names its source node,
  its destination node and its `delay_frames`. Delay sits on node inputs, per
  edge, because PDC (Phase 4) delays paths, not nodes.
- **Delay-line state lives outside the plan**, in `engine/core`, keyed by a
  stable node id that survives plan swaps. A swap that kept the same graph
  keeps its delay lines' contents; a fresh plan never starts a delay line
  from silence by accident.
- **Fixed maxima at session creation**: tracks, nodes, edges, scratch
  buffers, source slots, maximum delay. The builder refuses a plan that
  exceeds one, with the limit named. The audio thread never sees a plan
  larger than what was allocated for.
- **Per-block ramps.** Gain, pan, mute and solo are read from the `Track`
  atomics once per block and ramped linearly across the block from the
  value applied at the end of the previous one. Mute and solo ramp to and
  from zero the same way. No step change, so no click.
- Fades, crossfades and region gain are applied on the audio thread from
  the plan, at most two voices per track.
- Phase 2 runs the node list serially. The Phase 3 mixer adds node kinds,
  and the Phase 4 parallel scheduler and PDC run the same list.

**4.6 Playback reaches the hardware through `AudioBlock::outputs`.**
`HardwareOut` writes the bus's left and right into `outputs[monitor_out[0]]`
and `outputs[monitor_out[1]]`; every other output gets silence written, as
the M2 contract requires. The Phase 2 exit-criterion test runs against
`drivers/fake` with outputs captured, so it depends on ADR-0007 M3.

### 5. The session file is canonical, strict JSON; one parser for the project

The format and its rules are in `session-format.md`: integer samples for
every position, versioned with a single integer, migrated forward, refused
when newer.

- **I/O stays out of the model.** `common/model` turns bytes into a
  `Snapshot` and a `Snapshot` into bytes. `engine/control` writes those
  bytes through `common/platform`'s `write_file_durably(path, bytes)`, whose
  POSIX implementation does write, `fsync`, `rename`, directory `fsync`.
  All saves (main file, autosave, backups) are serialised on one saver
  thread owned by `engine/control`.
- **`common/json` is its own module** (target `sonora_json`, layer 0 beside
  `common/base`, standard library only, owner rt-engineer, ui-engineer
  reviews). It is not part of `common/base`, because `common/base` is RT-safe
  by definition (ADR-0007 §2) and a JSON DOM allocates. It may be linked by
  any module except `common/base` and `contracts/*`. This adds a module and
  edges to ADR-0007 §1's graph, an additive change under ADR-0007 §6.
- It keeps each number's lexeme, and parses with `std::from_chars`: integers
  into `int64_t` exactly, floats **straight into `float`** (no `double` in
  between, which would round twice). `std::from_chars` ignores the C locale,
  which `strtod` does not. It reports line and key path, and caps input
  size, nesting depth and DOM size (`session-format.md`).
- `common/model` uses it from the start; `ui/keymap` moves onto it in its
  own PR, which also fixes the keymap's `strtod` locale bug.

## Rationale

- **The worst outcome decides undo.** A command-pattern undo stores an
  inverse for every operation, written by hand. Each inverse is a chance to
  restore something subtly different, and the overwrite operation that
  punch-in uses touches up to four regions and their crossfades. A bug there
  is a take that is gone after undo. With snapshots, undo cannot restore the
  wrong thing, because it restores the thing itself. The price is memory,
  bounded by structural sharing and a byte budget, and measured before M8
  lands.
- **Undo during a take, safely.** The take in progress is not in the
  document until the disk thread reports it closed, and the document has one
  writer. An undo pressed while recording changes prior edits. The take then
  commits on top of the result. No race needs reasoning about.
- **Non-overlap is an RT bound and an AAF shape.** Layered regions make the
  number of voices per track unbounded and push layer resolution into the
  callback. Non-overlapping playlists with explicit crossfades are a
  `Sequence` with `Transition`s, which is what AAF stores. So the Phase 4
  round trip is a mapping, not a flattening.
- **Residency gating is what "without stopping the transport" costs.**
  Without the gate, an undo during playback swaps in a plan whose audio was
  evicted, and the operator hears a hole exactly where they are listening.
  With it, the operator hears the old edit for one read longer.
- **The retirement protocol is written down to the memory order** because
  "after the epoch has passed" admits implementations that are wrong only on
  a weak memory model or under a stall, which no listening test catches.
  The stated order has a short proof, a TSan test, and no timeout.
- **Fail loudly.** A session newer than the program is refused. A missing
  file is listed on screen at open. A crossfade without handle material is
  refused rather than shortened. A missing chunk is counted, not hidden. A
  plan larger than the session's maxima, or whose demand exceeds the pool,
  is flagged at build, not discovered as underruns.
- **Platform neutrality is kept by construction.** `apply` is pure and the
  model does no I/O. Durability goes through `common/platform` (ADR-0008).

## Consequences

- **`engine/core` gains a second disk thread and a cache.** The reader, the
  slot arrays and the pool are new RT-adjacent code: the slot load and the
  plan acquire are `SONORA_RT` and gated by `rt-safety-auditor`. The TSan
  stress test (§4.1) lands with them.
- **The take boundary moves in-band.** Punch-out closes a take while
  rolling through a boundary record on an SPSC ring from the audio thread to
  the disk thread; the capture ring is never reset (`timeline-model.md`,
  "Punch in"). This replaces the take-epoch handshake that `planning/BACKLOG.md`
  listed as the fix for the ring-reset race: with no reset there is no race
  to hand-shake around. The regression test for that race is still written
  first.
- **`RecordTake` waits for the disk thread.** The document applies a take
  only after the disk thread reports "take closed, length *N*". The length
  in the session is the length on disk, not a transport computation.
- **Crash recovery has a stated loss window.** The disk thread calls
  `fdatasync` on every open take file periodically, through `common/platform`
  (`session-format.md`, "Crash recovery"). This is a new backlog item for
  rt-engineer. Until it lands, the format makes no claim that audio survives
  a power cut.
- **Memory.** The chunk pool is a session-creation parameter. For example, 18
  stereo tracks, 16 k-frame chunks (64 KiB float), a 4 s window, one region
  per window: 2 × 13 chunks × 18 tracks × 2 plans = 936 chunks, about 59 MiB.
  A 2,000-region comp needs more, and how much is measured (below). Undo is
  64 MiB by default.
- **Two new modules**: `common/json` (§5) and `common/model` (ADR-0007 M8).
  `common/platform` exists or is created by whichever PR first needs it
  (ADR-0008); the session saver's durable write and the disk thread's
  `fdatasync` both go there.
- **The UI cannot edit its replica optimistically.** A drag in progress is
  interface state until the engine's operation arrives. At human rates over a
  socket this is assumed invisible (ADR-0007's measured round trip, p99 about
  160 µs in a container).
- **Undo history is lost at reload,** as in Pro Tools. Saving it would make
  the session file the replication log, and it is not worth the size.
- **Default timecode origin 01:00:00:00** for new sessions, so a
  latency-compensated `TimeReference` never wraps past midnight. A Phase 1
  recording with no session file keeps writing `TimeReference = playhead`.

## Unverified

Each names the measurement that settles it.

1. **Mixed disk throughput on a USB stick** (chunk size, residency window,
   locate bound). 18 tracks recording 24-bit/48 kHz while 18 play back, on
   the persistence partition of a physical stick. Chunk sizes 8 k and 16 k
   frames with coalesced reads, worst-case read latency. Researcher; result
   in `brain/research/`. Until it exists, 16 k frames and 4 s are
   placeholders.
2. **Durability of `rename` plus directory `fsync` through the persistence
   overlay** (`home/rw` on ext4). Kill a QEMU VM at random points during a
   save loop, 200 times; the file must always parse and be the previous or the
   next save. Researcher.
3. **Undo memory.** 18 tracks × 2,000 regions, 200 single-region edits on the
   longest playlist, and bulk edits (select-all nudge, a 2,000-region comp
   promoted), peak RSS against the 64 MiB budget. Over budget at a few dozen
   steps means chunked playlists. rt-engineer, before M8.
4. **Plan build time** for that session. Over about 20 ms moves the builder
   onto a worker owned by `engine/control`. rt-engineer, in the M8 PR.
5. **Worst-case `process()`** on the target machine with the UMC1820: 18
   tracks × 2 voices plus 18 inputs at quantum 128, with pool and plans
   pre-faulted. Before M8; needs hardware.
6. **Pool demand of a 2,000-region comp** at the placeholder chunk size,
   peak over window positions. Before M8.
7. **Undo-to-audible latency**, p99 and max, on a real stick while 18 tracks
   record. Before M8; needs hardware.
8. **`fdatasync` cost** on the stick while 18 tracks record, at the interval
   chosen: it must not cause a capture overrun. rt-engineer with the
   periodic-sync item.
9. **AAF fade representation**: whether libaaf reads and writes a fade on a
   lone clip through `SourceClip`'s fade properties, and what other DAWs
   emit. Researcher, before Phase 4.

## Update, 2026-10-13: hardware measurements move after the Complete DAW

The owner put the appliance and the UMC1820 at the end (`planning/ROADMAP.md`,
2026-10-13), while `common/model` (M8) is already being written. Items 5 and
7 say "before M8; needs hardware", and items 1, 2 and 8 need a physical
stick. That order can no longer hold. The decision above is unchanged; only
when its assumptions are checked moves:

- **Before M8 merges, as labelled proxies:** items 3, 4 and 6 as written
  (no hardware). Items 5 and 7 measured on the owner's laptop, under the
  fake backend and under WSL2 with onboard audio, reported as proxies.
- **In OS 1, on the UMC1820 and a physical stick:** items 1, 2, 5, 7 and 8
  for real.

What this costs if the late figures disagree: chunk size, residency window
and fdatasync interval are parameters, so a bad item 1 or 8 is retuning. A
bad item 5 (worst-case `process()` over budget at 18 tracks × 2 voices on
the target machine) would mean the voice model or plan shape changes, after
DAW 2 to 5 are built on it. That is the risk accepted by doing the product
first. The proxy figure for item 5 is the early warning: if it uses more
than half the block on the laptop, raise it before building on the plan.

## Update, 2026-10-13: a resync carries the undo window

Amends §2's "that snapshot is opaque bytes (the session file's canonical
JSON)". A session file holds one state, so a replica rebuilt from it could
not undo what the engine could, and the first `Undo` after a resync
desynced it, against §1's "engine and replica offer exactly the same undo
steps". The snapshot is now `common/model`'s `encode_window()`: the current
state with every undo step from the floor up and the pointer, sharing
tracks and playlists across steps as the engine's history does. It is still
opaque bytes to `contracts/api`. ADR-0007's M4 update, item 2, has the
detail and the rule for a window over the wire's cap.

## Update, 2026-10-13: playback on the M4/M7 control server

Decided by: architect. Binds the playback branch (PR #31, head f7df34e) to
the architecture that landed after it was written: M4 (#33), the Qt UI on
`common/model` (#35) and M7's process split (#38, main 3684c54). §4 and
`timeline-model.md` are unchanged. This decides who runs them.

**Context.** #31 was written against the old interactive loop in
`engine/app`. It keeps its own document (`TakeDocument`, a private
`model::History` it adds closed takes to) and drives `PlaybackController`
from that loop. Since then `EngineControlServer` owns the engine's
`model::Document`, which every interface replicates, and `sonora-engine` has
no interactive loop: `sonora-tui` and `sonora-ui` are separate processes on
the socket. Two consequences if #31 were merged as written: two documents,
one of them heard and the other shown and edited; and no thread left to
call `pump()`. Also, on main today the server's document never learns of a
take: closed takes are on disk and absent from every replica.

**Decision.**

1. **One document: the server's `model::Document`.** `TakeDocument` is
   removed. Its job becomes a function in `engine/control`
   (`closed_take.h`) that, when a take has closed, applies per armed track
   that wrote frames `AddSource` (`SourceKind::Recorded`, length and
   `time_reference` read from the closed file, as `place_file` did) and then
   `RecordTake` through `Document::apply(const Operation&)`, and returns the
   `ReplicationUpdate`s. The server sends each one to every client like any
   other update. `timeline-model.md`, "At punch-out", already said this.
   `PlaceRegion` with `SourceKind::Imported` is kept only for files
   `--place` puts on the timeline.
2. **The server's control thread is this ADR's document thread.**
   `EngineControlServer` owns the `PlaybackController` (a member, built
   in its constructor) and calls it only from the control thread. After
   every change to the document (`Apply`, `Undo`, `Redo`, `Load`, or a
   take added under item 1), it calls `show(document.current())`. In every
   loop iteration it detects the Recording to not-Recording edge (item 1)
   and then calls `pump()`. While the controller has anything waiting (a
   pending plan, a locate, a requested roll or take), the control thread's
   idle wait drops from 50 ms to 2 ms. No second thread touches the
   document or the controller, so neither needs a lock.
3. **`show()` stops writing the mix on every edit.** A fader moved through
   `ParameterChange` is a `Track` atomic that the document's mix does not
   hold yet. If `show()` applied the document's mix after every edit, it
   would snap every fader back on the next nudge. The plan is published on
   every change; `apply_mix()` runs only at start and after an update that
   changed mix state (`SetMix`, `SetMonitorBus`, `AddTrack`, `Load`, a
   `Snapshot`). Writing the end of a fader gesture into the document as
   `SetMix` is a separate item.
4. **`transport.play` uses the existing control API.** `transport.play`,
   `transport.toggle_play` and `transport.return_to_zero` are already
   `Engine` ids in `command_catalog()`. They arrive as `CommandRequest`,
   handled by `install_session_commands(..., &controller)` as in #31.
   There is no new message and no protocol bump. The state block's
   `TransportState::Rolling` and playhead already report the roll.
   - **Replies.** `transport.play` replies `Ok` when the request is
     accepted, because a roll that waits for residency loses nothing.
     `transport.record` and `transport.toggle_record` reply only when the
     gate resolves: `Ok` once the take has started, `Failed` with
     `take_record_error()`'s reason if it was refused, and `Failed`
     "cancelled" if a second press withdrew it. An `Ok` for a take that
     never started is a lost take the operator was told is recording.
   - **`FakeEngine`.** `engine_commands()` gains the three ids, with a
     scripted roll, so the `control` test's parity check holds. The
     conformance suite gains a play check: roll, playhead advancing, stop.
     The test files are under `contracts/api`, so ui-engineer and
     rt-engineer review the PR (ADR-0007 §6), but no wire format changes.
   - **Not in this port.** A locate to an arbitrary frame (a click on the
     ruler) needs a new catalog id (`transport.locate`, frame in
     `argument`), and "Locating…", bus peaks and playback misses in the
     state block need layout version 2. Both are contract PRs of their own
     after the port, not part of it. (Both landed: `transport.locate` in
     #123, the state block as layout 5 in the update of 2026-11-01.)
5. **The headless `--record` path uses the same server.** With `--place`,
   or whenever the session has playback, `run_headless` builds the
   `EngineControlServer` without a socket and drives it through
   `InProcessEngine` (arm, `transport.record`, wait for Recording, stop).
   `--place` files go into the server's initial document (an
   `Options::initial_document` snapshot built before `start()`). The
   `RESULT` line keeps `misses=`, read from the `Session`.
6. **Teardown.** After `AudioBackend::deactivate()` returns, `engine/app`
   calls `EngineControlServer::after_deactivate()`, which forwards to the
   controller (§4.1: everything retired is freed then, and only then). The
   server is destroyed before the `Session`, as now.

**What #31 keeps, moves and drops.**

- **Kept as written:** all of `engine/core` (plan layout, `PlanExchange`,
  `DiskReader`, `Session::process()` mixing, `RtCommand` locate,
  `Transport::locates_applied`, denormals); the `drivers/fake`,
  `drivers/jack` and `drivers/support/FrameClock` changes and the
  `audio_backend.h` comment; `WavReader`; the gain and pan-law ranges in
  `common/model` and `session-format.md`; `plan_builder`; `PlaybackController`;
  all of #31's tests; its amendments to §4.3 and §4.4 of this ADR; its
  ADR-0007 update on skipped periods; the postmortem; `verify-recording.py`;
  its backlog items.
- **Moved:** `TakeDocument::place_file` and `add_closed_take` to
  `engine/control/closed_take.{h,cpp}`, operating on `model::Document`
  (item 1). `--place`, `--arm N` and `--no-input-monitoring` stay as
  `engine/app` flags, wired through the server (item 5). Two outputs at
  `open()` (§4.6) stay.
- **Dropped:** the `TakeDocument` class, and everything #31 added to the
  interactive loop in `main.cpp` (bus meter drawing, the `was_recording`
  edge, `pump()` per frame), since the loop no longer exists. The bus
  meters return in `sonora-tui` and `sonora-ui` once the state block
  carries them.

**Rationale.** One document is the whole point of §2: what is heard, what is
shown and what undo removes must be the same state. Two would agree only
until the first edit made in the UI. The control thread already serialises
every request and owns the document. If the controller had its own thread,
that thread and the control thread would both need to read the document
and the controller, so a lock would be needed. A lock between the
controller and the thread that handles `transport.stop` puts a wait on a
take's stop path. Holding the record reply until the gate resolves follows
the product's first rule: an operator who sees "recording" must be
recording.

**Consequences.**

- A plan build on the control thread delays every command queued behind it
  for as long as the build takes, `transport.stop` included. This is
  Unverified 4 with a sharper cost. If a build is measured above 10 ms, the
  builder moves to a worker that `engine/control` owns, as §4 already
  allows.
- A take on N tracks is N undo steps, one `RecordTake` each, as in #31. In
  Pro Tools, one undo removes the whole take. A multi-track `RecordTake` is
  a `common/model` contract change and is filed as its own item.
- The engine now allocates ids in the document (the take's source and
  region). A replica that resolved an edit against the same `next_id` has
  that edit refused and taken back, loudly, by #38's existing path. Moving
  `resolve_*` into the engine (M7 note, item 6) removes the race.
- `PROCESS SPLIT PASS` in `tools/test-engine-entrypoint.sh` expects
  playback to be refused as `NotImplemented`. The port changes that step so
  playback is expected to roll.

**Unverified.**

- How long play and record take to start when the control thread polls
  every 2 ms. To measure: p50, p99 and max from the `transport.play` request
  to `Rolling` in the state block, against `drivers/fake`, with the window
  already resident and with a cold window. If p99 exceeds residency plus
  5 ms, the `DiskReader` wakes the control thread when a window becomes
  resident, instead of the thread polling.
- Plan build time on the control thread at 18 tracks × 2000 regions
  (Unverified 4). This is for rt-engineer, or the researcher if it is
  measured as a proxy on the laptop.

**Measured, 2026-10-13, in the port's PR (proxies).** `measure_playback`
(engine/control, built with the tests, run by hand), Release build in the
`sonora-build` container on the owner's Windows laptop (Docker Desktop,
WSL2). 18 tracks, chunk 16,384 frames, window 4 s, `drivers/fake` at
real-time pace, quantum 128. "Transport" is the `Session`'s own state, "block"
the state block, which adds the publisher's 10 ms period. Resident: play
again after a stop, record again right after a take (which first adds the
take to the document and replans). Cold: the first request to a server just
started, nothing resident and the plan not yet published; the files were in
the page cache.

| Request | Window | n | Transport p50 / p99 / max | Block p50 / p99 / max |
|---|---|---|---|---|
| play | resident | 200 | 2.3 / 2.5 / 2.5 ms | 9.3 / 10.1 / 10.1 ms |
| record | resident | 50 | 3.1 / 24.7 / 24.7 ms | 9.2 / 29.9 / 29.9 ms |
| play | cold | 30 | 6.6 / 10.7 / 10.7 ms | 10.2 / 20.1 / 20.1 ms |
| record | cold | 30 | 5.4 / 7.7 / 7.7 ms | 9.1 / 9.3 / 9.3 ms |

The resident play is the 2 ms poll and nothing else. The record p99 is the
replan after the previous take (36 operations, then the new plan adopted
before the gate opens). Nothing here exceeds residency plus 5 ms by a
margin that calls for the reader waking the control thread; to be measured
again on the stick (Unverified 1), where residency is not the page cache.

Plan build, `build_plan()` with registered sources, 18 tracks × 2,000
regions (36,000 segments, 88,704 chunk slots), n = 30: **77 ms p50, 96 ms
max**. That is over the 10 ms above which the Consequences move the builder
to a worker owned by `engine/control`. Every command queued behind an edit
of a session that size, `transport.stop` included, waits that long on the
control thread. Not done in the port; filed in `planning/BACKLOG.md` (DAW 1
detail) to land before sessions that size are edited.

**Alternatives rejected.**

- **Keep `TakeDocument` beside the server's document.** This merges #31
  with the least work. It was rejected because what plays would not be
  what the UI shows or undoes: an edit made in `sonora-ui` would never be
  heard.
- **`PlaybackController` on its own thread.** It would keep the control
  thread's latency free of plan builds. It was rejected because two
  threads would then write the controller and read the document, which
  needs a lock in the stop path, and the worker that §4 allows already
  addresses the build cost without one.
- **A new `TransportRequest` message (play, locate with a frame).** It
  would give typed arguments. It was rejected for this port because the
  catalog ids already exist and a new message type means a protocol bump.
  Locate-to-frame gets its own contract PR.
- **Reply `Ok` to `transport.record` on acceptance, report a refusal as a
  `Notice`.** This gives the same reply timing for every command. It was
  rejected because the UI would show a take as started and give the
  refusal only as a side line.

## Update, 2026-10-14: compound operations (Accepted)

Status: **Accepted by the owner, 2026-10-18**, with the architect's changes
and the owner's mix/plug-in undo decision. Implementation: PR #75.
Author: the model track (Track 2). Revised after the architect's review
(PR #70, "Accept with listed changes"). It amends §1 and §2. It also
replaces the 2026-10-13 control-server update's consequence "A take on N
tracks is N undo steps".

**Context.** An operation is the unit of three things: validation, one
revision on the wire, and one undo step. Several edits waiting in the UI
are by nature more than one operation:

- cut, copy and paste, and duplicate: a split at each edge, then the
  regions in between removed or placed, on every selected track;
- Shuffle: the same, with a `ShiftRegions` member (item 5) closing or
  opening the gap;
- dropping the fade hidden under a new crossfade, as Pro Tools does: a
  `SetFade` of length 0, then a `SetCrossfade` (BACKLOG Track 3 item 1);
- Alt-click on solo or mute: one `SetMix` per track;
- a take on N tracks: `AddSource` then `RecordTake` per track, which is
  N undo steps today where Pro Tools has one;
- ADR-0011's mix assistant, which applies an accepted plan as one
  operation. The compound gives that plan atomicity. Its undo follows the
  owner's decision of 2026-10-14 (item 4): the plan's structural members
  (`AddInsert`, sends) are one edit-undo step, and its value members
  (`SetMix`, `SetInsertParams`) are one step in the separate mix and
  plug-in undo history. ADR-0011 (Proposed) says "one undoable compound
  operation" and "ordinary, undoable parameter changes". Read against
  this update, that means two steps, one per history, from one operation.

Sending these operations one at a time has three defects. A refusal
halfway through leaves the first half applied. Every replica sees the
intermediate states, and the plan builder may publish one, so a cut can be
heard half done. And undo takes N presses. Grouping undo steps in
`History` fixes only the third defect.

**Decision.**

1. **A new operation, `Compound{ops, label}`.** Its members are ordinary
   operations, in order. There is no nesting and no recursion limit to
   set. An empty compound is refused.
   - **Type shape.** `common/model` splits the variant: `Member` is a
     variant of today's leaf operations, and `Operation` is those same
     alternatives plus `Compound{std::vector<Member> ops; std::string
     label;}`. The type, not a runtime check, stops a member from being a
     `Compound`. Every exhaustive visit (`apply`, `operation_name`,
     `is_undoable`, the codec, the server's kind checks) gains a case.
   - **`label`** is display-only: what the Edit menu and the Undo History
     show ("Undo Paste", "Undo Record"), since `operation_name` would say
     "Compound". `apply()` ignores it, and it is not part of an
     operation's equality. It is optional on the wire; when it is absent,
     each history shows `operation_name` of its own first member.
   - **ADR-0007 §6 classification.** This is an **additive** change to
     `common/model`'s public headers (`operations.h`, `op_codec.h`) and
     to `contracts/api` (the bump in item 6). It is reviewed by
     rt-engineer and ui-engineer under ADR-0007 §6, and `CONTRACTS.lock`
     is regenerated in the same PR.
2. **Validated as a whole, applied atomically.** `apply(before, Compound)`
   applies each member, with the existing pure `apply()`, to the result of
   the member before it. Member *k* therefore sees the ids, sources and
   regions that the earlier members created: `AddSource` and then a
   `RecordTake` that names that source, or `SplitRegion` and then
   `RemoveRegion` of its right part. If any member is refused, the whole
   compound is refused and the result is `before`. Each member's
   `apply()` already guarantees `check_invariants()`, so every
   intermediate result satisfies it. Only the final result is published.
   - **Error indices.** The refusal counts members from 1, for people:
     `operation 4 of 36 (RecordTake): <the member's own reason>`. The
     decoder reports a JSON path, counted from 0 as JSON paths are:
     `ops[3]: <reason>`. Both name the same member.
   - **Ids.** Whoever builds a compound (the client's resolver today, the
     engine for a take) gives its members strictly increasing ids,
     starting at `next_id`, and reserves every conditional id
     (`split_right_id` and the like) whether or not the member ends up
     using it. `fresh()` then holds member by member against the running
     state. Gaps in the id space are harmless. `add_closed_take()` reads
     `document.current().next_id` between applies today; it allocates all
     of a take's ids up front instead.
   - **Engine checks run per member, on the intermediate state.**
     `Document::apply` takes an optional member-check hook,
     `(const Snapshot& at, const Member& member) -> refusal`, called before
     each member with the state the earlier members produced. A refusal
     from the hook refuses the compound like a model refusal, with the same
     member-numbered error. A pre-scan against `before` would be wrong: in
     `AddTrack` then `AddInsert` on that track, the hostable check would
     find no track. The engine server and `FakeEngine` pass the same kind
     of hook, so the fold lives in one place.
   - **Atomicity covers the document only.** The plugin hosts that the
     compound's `AddInsert` and `ReplaceInsert` members need are started
     after the compound is published, make-before-break as in ADR-0012.
     A host that fails to start shows as faulted, exactly as after a
     single `AddInsert`. Nothing in the document is rolled back for it.
3. **One revision, one update, one step.** An applied compound raises the
   revision by one, however many members it has. It replicates as one
   `ReplicationUpdate::Kind::Op` that carries the compound's bytes. A
   replica applies it with the same function and never sees an
   intermediate state. The undo floor rule is the same as for any
   operation. **Residency (§4):** a compound publishes one plan, so it
   costs one residency wait; §4.2's newest-wins rule is unchanged, and the
   record gate after a take compound behaves as it does today, because
   `show_document` already runs once per take.
4. **Undo: at most one step per history, never one per member.**

   **Decided by the owner, 2026-10-14: mixer and plug-in value changes
   have their own undo history, separate from the edit undo,** as Pro
   Tools keeps mix and plug-in undo apart from edit undo. Edit Ctrl+Z
   keeps covering only edit-state operations, and the model's edit undo
   table (`timeline-model.md` "What undo covers", ADR-0012 §6.3) stays as
   it is: `SetMix` (fader, pan, mute, solo), `SetInsertBypass` and
   `SetInsertParams` remain non-undoable *for the edit history*, and go to
   the mix and plug-in history instead. `SetInsertActive` stays in the
   edit history, as the table has it. `SetInsertState` is written at save,
   after `GetState`, and is not an operator's gesture, so it makes no step
   in either. The mix and plug-in history itself is a separate piece of
   work (BACKLOG Track 2 item 6), not part of this update's
   implementation.

   **Edit history.** A compound is edit-undoable when at least one member
   is. It then pushes one edit step, the final `EditState`, charged the
   exclusive bytes against the previous step as any step is (§1). The
   intermediate states are never steps. Edit undo restores the
   `EditState` and nothing else (§3).

   **A compound that mixes edit and mix members is accepted, and makes
   one step in each history:** one edit-undo step for its edit members,
   as above, and one mix-undo step for its mix members, both carrying the
   compound's `label`. Each history undoes its own step independently, as
   Pro Tools' two undos do. Refusing mixed compounds was rejected, because
   the edits that need them most are mixed by nature: the mix
   assistant's accepted plan (`AddInsert` and its parameter values, a send
   and its level), and a new track or insert created with its starting
   values. Refused, they would have to go as two operations, and replicas
   and the plan builder would see, and could play, an insert at its
   default values between them: the very defect the compound exists to
   remove (Context). The split costs nothing in the model: `apply()` is
   unchanged, and only the two histories each record what belongs to
   them. An edit undo that removes an insert leaves its values in its
   `InsertMix` entry (ADR-0012 §6.3), so a redo restores them; how a mix
   step whose insert is gone is shown or skipped is for the mix history's
   design (BACKLOG Track 2 item 6). Until that history exists, mix members
   make no step, as today.

   Therefore:
   - a multi-track take (`AddSource`×N, `RecordTake`×N) is one edit-undo
     step, and undoing it leaves its N sources in the pool, as undoing one
     `RecordTake` already does;
   - Alt-click on solo or mute (`SetMix`×N) makes **no** edit-undo step,
     and one mix-undo step once that history exists. The compound gives it
     atomicity and one update. Today it makes N revisions and no undo step
     at all, since `SetMix` is not undoable. Edit Ctrl+Z must not offer to
     undo it. When this lands, the BACKLOG line "it should be one compound
     undo step" for Alt-click is corrected to "no edit-undo step; one
     mix-undo step (Track 2 item 6)".
5. **A size limit: 4,096 members as the target, 64 for now** (measured
   below). 4,096 is about 200 members per track
   on 18 tracks. That covers an 18-track take (36 members), Alt-click
   (18), and a cut or paste over a selection of a few hundred regions. The
   decoder refuses more, and so does `apply()`, each naming the limit.
   `contracts/api`'s `kMaxMessageBytes` also caps the encoded request.
   - **Cost, and the first remedy.** Applied naively, each member copies
     the playlist it edits (§1), so *k* members on one *n*-region playlist
     copy O(*k*·*n*) regions on the document thread, and `transport.stop`
     waits behind that work (2026-10-13 update, consequences). The
     architect's estimate for 4,096 members on one 2,000-region playlist
     is hundreds of ms, one to two orders of magnitude over the 10 ms
     build threshold of the 2026-10-13 update. Lowering the limit to fit
     would leave about 100 to 200 members, below a cut over a few hundred
     regions. So the fold copies each playlist **once per compound** and
     edits that copy in place for the later members: until the result is
     published, the fold is its only owner. The result is identical, and
     `apply()` stays observably pure. The limit is lowered only if the
     measured cost is still over 10 ms after that.
   - **Measurement.** The implementing PR measures 4,096 members on a
     2,000-region playlist (and an 18×2,000 session) before it merges,
     reviewed by rt-engineer, and reports the numbers in this ADR. It is
     not required before this update is accepted: the limit is a
     parameter, the cost is control-thread latency and not RT safety or a
     lost take, and a compound is never worse than the same members sent
     one by one.
   - **Measured, 2026-10-18** (`feature/compound-op`,
     `common/model/tests/measure_compound.cpp`; Release, in the
     `sonora-build:trixie` container on the owner's i7-11800H under Docker
     Desktop; best of 5 runs). Each region is 1,000 frames with a gap after
     it, and there are no crossfades. `Document::apply` covers encoding, the
     fold and the history step; "one by one" is the same members sent as
     separate operations.

     | Compound | Playlists × regions | `Document::apply` | One by one | Decode |
     |---|---|---|---|---|
     | 4,096 members, split then remove (cut-like) | 1 × 2,000 | 1,169 ms | 1,441 ms | 2.8 ms |
     | 4,096 `SetFade` | 1 × 2,000 | 673 ms | 978 ms | 3.9 ms |
     | 4,096, split then remove (cut-like) | 18 × 2,000 | 1,071 ms | 1,445 ms | 3.1 ms |
     | 4,096 `SetFade` | 18 × 2,000 | 823 ms | 1,070 ms | 4.4 ms |
     | 1 / 16 / 64 / 256 / 1,024 `SetFade` | 1 × 2,000 | 0.21 / 2.4 / 9.1 / 38 / 190 ms | 0.21 / 3.5 / 14 / 64 / 254 ms | |

     **The cost is over 10 ms after the remedy, by about 100 times at
     4,096.** Copying each playlist once saves 19 to 31 %.

     **Profile** (rt-engineer, PR #75 co-review; gprof and a component
     benchmark on 2,000 regions with no crossfades). About 75 % of a
     member's cost is hash maps over every region, rebuilt and thrown away
     on each member:

     | Per call | µs |
     |---|---|
     | `playlist_problem`, whole playlist | 181 |
     | of which its `unordered_map` index, one node a region | 114 |
     | of which `region_problem` × *n* | 40 |
     | `sort_crossfades` with no crossfade to sort | 124 |
     | the playlist copy, first member only | 110 |
     | fold steady state, a `SetFade` member | 259 |

     With no per-member validation at all, 4,096 `SetFade` members cost
     5.2 ms, but a cut-like compound still costs 304 ms. That is because
     `overwrite()` rebuilds the region vector for every member, and
     `drop_broken_crossfades()` builds two maps over every region even
     when there is no crossfade. Local validation alone is therefore not
     enough. The operators have to be range-local too.

     **The rule applied: an interim limit of 64.** This is item 5's own
     rule. `sort_crossfades` (with fewer than 2 crossfades) and
     `drop_broken_crossfades` (with none) now return early. With that,
     the cost at 64 members, measured 2026-10-18 the same way, is:

     | 64 members | 1 × 2,000 | 18 × 2,000 | One by one |
     |---|---|---|---|
     | split then remove (cut-like) | 6.4 ms | 7.5 ms | 11.7 / 11.8 ms |
     | `SetFade` | 5.1 ms | 6.6 ms | 9.6 / 10.1 ms |

     That table is the quietest run, and on it every case is under 10 ms.
     The rt-engineer's re-review (PR #75) ran the same build three times
     on a noisier host. It measured 64 members on 18 × 2,000 at 9.6 /
     13.7 / 17.4 ms (`SetFade`) and 11.2 / 15.5 / 17.6 ms (cut-like),
     best of 5 each. On 1 × 2,000 it measured 6.0 to 12.8 ms. **On a
     typical run the 18-playlist case is about 10 to 11 ms**, not 7.5:
     each of the 18 playlists is still copied once (about 110 µs) and
     fully re-validated. Item 8's work starts from these numbers. The
     limit stays 64 regardless, because the compound still costs about
     half what the same members cost sent one by one.

     64 covers every sender that exists today:
     a take (two members a track, and `add_closed_take()` splits a take of
     more than 32 armed tracks into compounds in track order), Alt-click on
     18 tracks, and a crossfade with its `SetFade`. Cut, copy and paste are
     not built yet.

     **4,096 stays the target.** It is reached by exact local validation
     inside a fold, of the touched range ±1 and its crossfades, not deferred
     to the fold's end, so that "fold == members one by one" holds. A test
     checks the local verdict against `playlist_problem`'s after every
     member. The other part is range-local `overwrite()` and
     `drop_broken_crossfades()` (BACKLOG Track 2 item 8). The limit is
     raised only to what that measures under 10 ms. A bound on members ×
     regions was rejected: the same Cut would be accepted on one session
     and refused on another, and the interface cannot know which.

     **Raising the limit needs a `kProtocolVersion` bump.** The limit is
     part of what a decoder accepts. A replica built with a lower limit
     cannot decode a larger compound, and would resync for ever.
   - **Shuffle.** On a long playlist it would need one `MoveRegion` per
     later region, which can pass the limit and costs O(*n*²). When
     Shuffle lands it adds a resolved `ShiftRegions{playlist, from,
     delta}` operation, used as a member of Shuffle's compound.
6. **Wire encoding and `kProtocolVersion` 4.** In `common/model`'s op
   codec, a compound is `{"op": "Compound", "ops": [<member>, ...],
   "label": "<text>"}`, `label` optional. Each member is encoded exactly
   as it would be on its own, so the encoding stays canonical and reuses
   the strict reader. `contracts/api` carries operations as opaque bytes
   (§2), but the replica cannot apply what it cannot decode, so
   **`kProtocolVersion` goes from 3 to 4**. A client and an engine that
   disagree are refused at connect, before any other message, by
   `version_mismatch()`, naming both versions ("the engine speaks control
   protocol version 4 and this interface speaks version 3") and telling
   the operator to install an interface and an engine from the same
   build. The rule in `messages.h` becomes "bump on any change to a
   message's encoding, **or a new operation kind the engine may send**",
   and the version history there gains "4 (ADR-0009 2026-10-14):
   `Compound`". The conformance suite and `FakeEngine` use the constant
   and follow it.

   **The label reaches every undo history** (PR #75 ui-engineer review),
   so "Undo Paste" needs no protocol 5 later:
   - each `History` step keeps a label, the `display_name()` of the
     operation that made it: a compound's `label`, or with none its first
     edit member's kind, and a single operation's kind;
   - `undo_label()` and `redo_label()` give the Edit menu its text;
   - `ModelUpdate` gains `label`, set on Undo and Redo to the step moved
     over. A replica whose own step has another label has lost track, and
     resyncs. This is part of protocol 4.
7. **Session file: unchanged. Undo window: format 3.** Operations are not
   saved, and `sonora_session` stays as it is. The window
   (`encode_window`) holds states, and from `kWindowFormat` 3 also each
   step's label, so a replica rebuilt from a resync offers the same undo
   and redo text as the engine. It rides on protocol 4.
8. **The engine.** `engine/control` decodes and applies a compound like
   any other operation. Wherever the server checks an operation's kind, it
   checks each member, through the hook of item 2:
   - a member that would start a plugin host makes the whole compound
     `NotImplemented` while hosting is off;
   - a `SetMix` or `SetMonitorBus` member counts as a mix change.

   **A take is never all-or-nothing.** `add_closed_take()` works in this
   order:
   1. For each armed track, `source_of()` reads the take's file. A file
      that does not read is found here, before any operation exists, so
      it is not a model refusal. Each failed track gets its own notice
      naming the track and the file.
   2. One compound is built of the tracks whose file read: each one's
      `AddSource` and `RecordTake`, in track order, with every id
      allocated up front (item 2). It is applied once, labelled "Record".
   3. If `apply()` refuses that compound, which for engine-built members
      is a model bug, each good track's `AddSource` is applied on its own.
      That puts the audio in the session's pool, so a save records it and
      nothing is lost. The engine then says so loudly, as an error and not
      a side line: the take is on disk and in the pool, but not on the
      timeline, and the refusal's reason. It goes out as `TakeFailed` with
      the take-failed health flag, and also as a `Notice`. The alert
      clears on the next clean take, which while recording on the roll can
      be the next tick; the Notice stays for the session, because the take
      is still not on the timeline then.

   One bad file therefore costs one track, never the take. Today the
   tracks before the bad one land and the ones after it are dropped, and
   the notice "take N is on disk but not in the session" is wrong for the
   tracks that did land. The per-track notices replace it. `FakeEngine`
   gets the same member checks.

**Consequences.**

- Each UI item listed under Context sends one operation. The client still
  allocates the ids for every member from `next_id`, under the rule of
  item 2. The race in the 2026-10-13 update's consequences is unchanged.
- A refused compound costs the work of the members applied before the
  refusal. That work is discarded and nothing is published.
- An interface and an engine from different builds no longer connect
  (item 6). That is the failure the product prefers to a resync loop.
- `timeline-model.md` adds `Compound`, with this rule, to its operations
  table.

**Alternatives rejected.**

- **A history group (`begin_group`/`end_group` in `History`).** It gives
  one undo step, but N revisions and N updates. Replicas and the plan
  builder would see every intermediate state. A refusal halfway through
  would leave the first half applied, and the only way to roll it back
  would be an undo that also has to be replicated.
- **The engine resolving each command into its own operation**
  (`Cut{...}`, `Paste{...}`, `SoloAll{...}`). `apply()` would take on edit
  modes' logic, and every new edit would need a new operation and codec
  entry. The compound composes the existing, tested operations. A
  purpose-built operation is still right where cost demands it
  (`ShiftRegions`, item 5).
- **A multi-track `RecordTake` only.** It fixes takes and nothing else on
  the list.
- **No `kProtocolVersion` bump.** The encoding of every message is
  unchanged, but an older interface's decoder refuses `Compound` as "not
  an operation this version knows". `Replica::receive` drops sync, the
  client resyncs from the window (which still decodes), and the next
  compound fails again: a resync loop showing stale state between
  resyncs. A newer interface on an older engine would have every cut,
  paste and Alt-click refused one request at a time. Both are worse than
  one refusal at connect.
- **An all-or-nothing take.** One bad file would drop all N tracks,
  whose audio is on disk but referenced by no session and not recorded
  by a save: a lost take, the worst outcome this product has.
- **Lowering the member limit as the first remedy for cost.** It would
  fall below the edits the compound exists for (item 5).
- **Pre-scanning members against `before` for the engine checks.** It
  misjudges any member that depends on an earlier one (item 2).
- **Refusing a compound that mixes edit and mix members.** It would keep
  each compound in one history, but it splits the mix assistant's plans
  and new inserts with their starting values into two operations, and so
  brings back the intermediate state the compound exists to hide (item 4).

## Note, 2026-10-18: the compound limit is 256, `kProtocolVersion` 5

Author: the model track (Track 2), BACKLOG Track 2 item 8. It applies the
remedy that item 5 of the compound update names. It changes no decision of
that update: the rules are the same, and only the limit's value and the
protocol version move. Reviewed by rt-engineer and ui-engineer under
ADR-0007 §6, because it changes what a decoder accepts.

**What changed.**

- **Exact local validation inside a fold.** Each playlist operator records
  the index ranges of the regions it changed, and the ids it removed
  (`common/model/src/local_edit.h`). After each member, `edit_playlist()`
  checks only those regions and one either side: `region_problem`,
  adjacency, every crossfade on any of them or on a removed region, and
  the heard-fade rule. It checks in `playlist_problem()`'s order and with
  its words, so the refusal is the same text with the same indices. The
  check runs after every member and is not deferred to the fold's end, so
  "fold == members one by one" still holds. Id uniqueness is not
  re-checked, because `fresh()` already guarantees it. The playlist was
  valid before the member, so nothing outside the touched neighbourhood can
  have become invalid.
- **Range-local operators.** `overwrite_range()` finds the first region
  that meets the range by binary search over region ends, and visits only
  the regions that meet it. It then splices their replacements in place,
  with at most one shift of the tail. `drop_broken_crossfades_near()`
  decides only the crossfades on a changed or removed region, and reads
  what the crossfades before them left on those regions, as the
  whole-playlist rule does. `fit_fades_near()` fits fades only on the
  touched neighbourhood. New crossfades are inserted in region order, so
  the per-member `sort_crossfades` and its map over every region are gone.
- **The audit.** A build with `SONORA_MODEL_AUDIT` runs every playlist
  edit twice: through the whole-playlist operators and `playlist_problem()`
  kept from before, and through the range-local ones. It aborts unless the
  two give the same verdict, in the same words, on the same playlist.
  Debug builds of `sonora_model` audit. So does `sonora_model_audited`,
  which every `common/model` test links, so CI's Release, TSan and ASan
  runs audit too. `model_local_edit` is a randomized property test of that
  audit: 1,500 compounds of 1 to 48 members on dense, crossfaded mono and
  stereo playlists, with every member checked. The fold must also equal
  the members applied one by one, and a refused member must refuse the
  compound with its own reason. `model_compound` and `model_random_edits`
  run under the same audit.

**Measured, 2026-10-18** (`measure_compound`, Release, in
`sonora-build:trixie` on the owner's i7-11800H under Docker Desktop). The
host was checked quiet: no container but the owner's long-lived database
before or after each run. These are three runs, best of 5 each; they are
spread from lowest to highest across the runs. The shapes are the same as
in item 5. `measure_compound` skips sizes above the limit, so the
1,024 and 4,096 rows were measured with the limit set to 4,096 for the
run.

| Members | 1 × 2,000 cut-like | 1 × 2,000 `SetFade` | 18 × 2,000 cut-like | 18 × 2,000 `SetFade` |
|---|---|---|---|---|
| 64 | 0.64 to 0.71 ms | 0.10 to 0.11 ms | 1.84 to 1.88 ms | 1.13 to 1.21 ms |
| 256 | 2.15 to 2.33 ms | 0.26 to 0.27 ms | **3.79 to 3.87 ms** | 1.33 to 1.39 ms |
| 1,024 | 8.12 to 8.23 ms | 1.11 to 1.13 ms | 10.90 to 11.80 ms | 2.15 to 2.25 ms |
| 4,096 | 21.87 to 23.04 ms | 5.00 to 5.83 ms | 40.77 to 43.17 ms | 5.77 to 6.07 ms |

That is `Document::apply`: the encoding, the fold and the history step.
For comparison, at 4,096 members before this change the cut-like case was
1,071 to 1,169 ms and the `SetFade` case 673 to 823 ms (item 5). The same
members sent one by one now cost 98 to 101 ms at 1,024 and 389 to 396 ms
at 4,096 on 18 × 2,000. Decoding 4,096 members takes 2.4 to 3.9 ms, and
the largest encoding, 4,096 `SetFade`, is 714 KB, well inside
`kMaxMessageBytes`.

**The tail, 2026-10-18** (PR #85 review). Best-of-5 hides the slow runs,
so every `Document::apply` sample was kept (`measure_compound --raw`) in
three more invocations, 15 samples a cell, on the code after the review
fixes and the same machine. The host was quieter at the start (1-minute
load 0.96) than at the end (4.81), so these cells are less quiet than the
table above; their best is from level with it to about 20 % above it.
Best / median / worst:

| Members | 1 × 2,000 cut-like | 1 × 2,000 `SetFade` | 18 × 2,000 cut-like | 18 × 2,000 `SetFade` |
|---|---|---|---|---|
| 64 | 0.62 / 0.68 / 0.94 ms | 0.11 / 0.12 / 0.29 ms | 2.14 / 2.29 / 2.49 ms | 1.28 / 1.55 / 2.10 ms |
| 256 | 2.41 / 2.52 / 3.27 ms | 0.28 / 0.29 / 0.38 ms | **4.24 / 4.70 / 7.35 ms** | 1.46 / 1.68 / 2.24 ms |
| 1,024 | 8.60 / 9.87 / 13.87 ms | 1.15 / 1.31 / 1.79 ms | 12.11 / 12.70 / 13.50 ms | 2.82 / 3.03 / 4.37 ms |
| 4,096 | 23.78 / 24.80 / 32.72 ms | 5.93 / 6.64 / 8.36 ms | 44.01 / 47.89 / 70.91 ms | 6.56 / 7.47 / 15.98 ms |

At 256 on 18 × 2,000 the median is 4.7 ms and the worst of 15 is 7.35 ms,
still under 10 ms; the worst is about 1.7 times the best, inside the 1.5
to 2 times busy-host factor below. At 1,024 even the best is over budget.

**The limit is 256.** That is the largest measured limit under 10 ms on
18 × 2,000, by item 5's rule. 1,024 measured 10.9 to 11.8 ms on the
cut-like shape. The quiet-host margin at 256 (3.8 ms, about 2.6 times
under the budget) covers the 1.5 to 2 times slowdown that the rt-engineer
measured on a busy host for PR #75. 512 was not measured. It would
probably come to about 6 ms quiet, but busy-host runs could put it at
10 ms or more.

256 covers an 18-track take (36 members) and Alt-click on 18 tracks (18).
It also covers a cut or paste across 18 tracks of about ten regions each,
with the two edge splits on every track. Cut, copy and paste can land on
it (BACKLOG Track 3 item 1).

**What still costs at 4,096.** `SetFade` is now about 1 µs a member. A
cut-like member is about 10 µs on a 2,000-region playlist, almost all of
it the O(*n*) shift of `Playlist::regions` on each split and removal, since
a region moves with its name and channel vector. Local validation cannot
remove that cost. Going past about 1,000 members would take one of two
things, each needing its own ADR: region storage that splices in
O(log *n*), which changes the public `Playlist`, or members that act on
many regions at once (`ShiftRegions` from item 5, a multi-region remove).
That is BACKLOG Track 2 item 8a. The target of 4,096 stands.

**`kProtocolVersion` 5.** A version-4 decoder refuses a compound of more
than 64 members. So a version-4 interface on this engine would resync on
every large paste, and fail again on the next one. With the bump, a client
and an engine from different builds refuse each other at connect, with
both numbers, as item 6 set out. Nothing else on the wire changes.
`messages.h` records "5: a compound may hold up to 256 members".

**Also changed.** `engine/control`'s `test_closed_take` covered splitting
a take of more than `kMaxCompoundMembers / 2` tracks into several
compounds. That now needs 130 tracks' files. While the limit is over twice
the test's 40 tracks, the test instead checks that a 40-track take lands
as one compound. The split path in `add_closed_take()` is kept, but no
test exercises it now.

## Update, 2026-11-01: state block layout 5, "Locating…", bus peaks, misses

Decided by: rt-engineer, on the recommended default. Lands the second half
of the playback update's item 4, "Not in this port". `transport.locate`
with the frame in `argument` merged in #123. That update wrote "layout
version 2". Since then ADR-0012 and ADR-0020 took 2, 3 and 4, so this is
`kStateLayoutVersion` **5**.

**Decision.** The state block (`contracts/api/state_block.h`) gains three
things. All of them are written by the publisher thread, and none by the
audio thread.

1. **`kTransportLocating`** (`TransportFlag` bit 3). It is set while
   `PlaybackController::locating()` holds a locate back because its window
   is not yet resident. The control thread hands the flag to the publisher
   after every `pump()` (`StatePublisher::set_locating`). The flag says
   that a locate is waiting. When to say so is up to the interface. §4.4
   says the interface shows "Locating…" once the flag has stood for
   100 ms, timed on its own clock, as `engine_alive()` is. A locate that
   resolves inside the first pump never sets it.
2. **`StateHeader::playback_misses`**, under the seqlock. It holds
   `Session::playback_misses()`, truncated to 32 bits like `xruns`. It
   counts the spans played as silence because their chunk was not
   resident, and it only rises during an engine run. Each one is an
   audible hole. An interface shows the count beside xruns, and shows it
   red when it rises.
3. **`StateHeader::bus_peaks[2]`**, the monitor bus's left and right
   peaks. Each is a `PeakRing` with the same slot format and tick tag as a
   track's, outside the seqlock for the same reason. The publisher becomes
   the only caller of `Session::take_bus_peak()`, as it already is of
   `Track::take_peak()`. `StateView::take_bus_peak(side)` keeps a cursor
   per reader for each side, so two interfaces metering the bus do not
   take each other's peaks.

`StateWriter::publish()` gains a trailing `const float* bus_peaks` (two
entries). A null pointer publishes silence, so a writer with no bus, such
as `FakeEngine` today, still tags every slot.

**No `kProtocolVersion` bump.** No message changes. `Hello` already
carries `state_layout_version`, and `wire::version_mismatch()` already
refuses a client of another layout at connect, naming both numbers.

**Consequences.** `state_header_bytes()` grows by two 512-byte rings. The
block is still sized once, when the session is created. A publish stores
two more slots and one more word inside the section. `sonora-tui` shows the
bus meter, the miss count and "Locating…". The Qt bus meter is its own
ui-engineer item, with screenshots.

## Update, 2026-11-01: record-ready, state block layout 6, protocol 37

Decided by: rt-engineer, on the owner's requirement of 2026-11-01 and its
recommended default.

**Context.** In Pro Tools, pressing Record does not start a take. The
transport goes *record ready* and the Record button blinks. The take starts
when the operator presses Play, and Stop takes record-ready back. Our
`transport.record` starts a take at once, which is Logic's R. Logic keeps
that behaviour. The Pro Tools profile gets the two-step behaviour.

**Decision.**

1. **Record-ready is engine state.** It is a control-thread `bool`, owned
   by the server like `take_`. The publisher shows it as
   **`kTransportRecordReady`** (`TransportFlag` bit 4), so every interface
   and the TUI show the same blinking Record. The flag is handed over as
   `kTransportLocating` is, through an atomic (`StatePublisher::
   set_record_ready`). The audio thread never reads it and never writes
   it.
2. **`transport.record_ready`** toggles it. It is installed with the
   playback controller, because only Play starts what it prepares.
   - It is refused when no track is armed, with "no track is armed".
   - It is refused while the transport rolls, or while a play waits for
     its window or its plug-ins, with "stop first".
   - During a take it does nothing and answers Ok, as a second
     `transport.record` does.
   - With a take waiting behind the residency gate, it withdraws that
     take, as `transport.toggle_record` does.
3. **Play while ready starts the take.** This covers `transport.play` and
   `transport.toggle_play` with the transport stopped. Each runs the start
   path that `transport.record` runs: `MayRecord`, the residency gate, a
   loop take from its pre-roll (ADR-0016 §1), a punch from A less the
   pre-roll, and the ADR-0016 C3 refusal. A range argument is ignored then.
   The take is the document's punch range, not the selection's play range.
   The flag clears once the take is asked for, whether it starts or is
   refused. At the server, such a Play counts as a take start
   (`starts_a_take`). So it is refused during a latency measurement and
   during a Save As copy. It never waits for plug-ins, because capture is
   pre-insert. Its reply waits on the gate, as a Record's does.
4. **What cancels it.** `transport.stop`, `transport.return_to_zero` and
   `transport.locate` cancel it. So do `transport.record`,
   `transport.toggle_record` and `transport.punch`, each of which starts a
   take itself. A session open (`Load`) resets it: per-session state does
   not cross into another session. Disarming every track leaves the flag
   up. The Play that follows is then refused with "no track is armed", as
   a Record would be.
5. **Keymaps.** The Record *button* arms, as in Pro Tools. The keys
   follow Avid's guide. Command+Space and F12 are listed under "Start
   record", so in `protools.json` they stay `transport.record`, a take at
   once. Keypad 3 is "Record Enable" in the Transport keypad table, the
   mode the profile assumes, so it is `transport.record_ready`. Space stays
   `transport.toggle_play`. `logic.json` keeps R on `transport.record`. The
   keymap essentials count `transport.record_ready` in the start group,
   because every shipped engine installs Play with it. The Qt Record button
   sends `transport.record_ready` under Pro Tools in a follow-up PR.

**Versions.** `kStateLayoutVersion` **6**, for the new flag. Layout 5 is
on main. `kProtocolVersion` **37**: Play now means something else when
ready, and there is a new command. It follows 36 (#226). Neither the session
format nor the window format changes, because the flag is not saved.

**Consequences.** The FakeEngine and the conformance suite carry the same
rules ("record-ready"), so FakeEngine, InProcessEngine and SocketEngine are
held to them. The TUI shows "REC READY". The Qt Record button does not
blink yet; it is a follow-up for the ui-engineer, with screenshots.

## Alternatives rejected

**Command pattern with hand-written inverses, over a mutable model.** It is
the textbook design and the lightest on memory. Rejected because every
inverse is a place for undo to restore the wrong thing, and the operations
Phase 2 needs most (overwrite on punch-in) are the hardest to invert. It also
needs a lock or a copy whenever a save or a plan build reads a model being
edited. Its memory advantage is real but bounded here by sharing and a byte
budget.

**Full copy of the session per edit.** Simple and correct. Rejected on cost:
proportional to the session, not to the edit, and a 36,000-region session
would copy megabytes per nudge.

**Undo bounded by a step count.** Simple, and what the first draft said.
Rejected in review: memory per step varies by orders of magnitude between a
nudge and a bulk edit, so a step count bounds nothing.

**Replicas evicting undo by their own accounting.** No extra field on the
wire. Rejected because two processes counting bytes differently would offer
different undo depths, and an undo the UI offers that the engine cannot do
is a lie on screen.

**Replicating snapshots or diffs instead of operations.** No `apply`
function to keep identical on both sides. Rejected because ADR-0007 M8
already chose operations, a diff of a value is larger than the operation
that made it, and the snapshot path is still there as the resync fallback.

**Per-track playback rings of rendered audio (Ardour's butler model).**
The audio thread does less: no fades, no region lookup. Rejected because the
ring holds audio rendered for the old plan, so an edit or an undo during
playback has to overwrite the ring ahead of a reader consuming it. That is a
race in exactly the path the exit criterion exercises, or a hole in the
audio if it is cleared instead.

**A lock-free hash table of chunks.** One shared lookup structure for every
plan. Rejected in review: open addressing with deletion under concurrent
readers is the hardest lock-free structure to get right, and a probe
sequence is an unbounded loop on the audio thread. A per-plan slot array is
an index computation and one atomic load.

**Queuing every gated plan.** Each edit audible in order. Rejected: a burst
of edits becomes a backlog of reads for plans nobody will hear.

**Freeing retired plans after a timeout when the callback stalls.** Memory
returns sooner. Rejected: a stall is exactly when a callback may still be
inside the old plan, and a use-after-free on the audio thread during a
take is worse than holding a few plans.

**Publishing the plan immediately and letting the reader catch up.** No
gate to build. Rejected: the hole it causes is audible and lands where the
operator is listening.

**Overlapping, layered regions (Logic-style).** More flexible, and the
model Logic users know. Rejected for Phase 2 because it makes voices per
track unbounded on the audio thread and has no direct AAF representation.
Comping (Phase 3) uses playlists, which this model already carries.

**Encodings.** XML: the same parser cost as JSON, more bytes, no benefit;
the keymap already set the JSON precedent. SQLite: atomic incremental
writes, but not diffable, a new dependency in a frozen package set
(ADR-0002), and incremental writes do not matter at about 4 MB. A custom
binary format: fastest, and opaque the day an engineer needs to rescue a
session by hand. YAML and TOML: ambiguous typing (YAML) or a poor fit for
deep arrays (TOML), each a larger parser.

**A lenient reader for newer files.** It would let an older image open a
newer session after an A/B rollback. Rejected because it shows a session
that is not the one on disk, and its writer would delete whatever it
skipped. The pre-migration backup solves the rollback case instead.

**A second private JSON parser in `common/model`, or JSON inside
`common/base`.** No new module. Rejected: two strict parsers drift into two
dialects, and `common/base` is RT-safe by definition, which a DOM is not.

**Building the plan in `engine/core`.** Keeps plan layout and builder
together. Rejected because ADR-0007 §2 puts the builder in `engine/control`,
and moving it would put model-reading code in the module whose callees may
be on the audio thread.
