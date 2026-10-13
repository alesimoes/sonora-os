# ADR-0009 — Timeline as immutable snapshots, operations as undo and replication, residency-gated plan swap

- Status: Accepted (engine owner review 2026-10-13, changes applied)
- Date: 2026-10-12; revised 2026-10-13 after the engine owner's review
- Decided by: architect. Rests on one owner decision, 2026-10-12: Phase 2
  playback starts with a fixed 2-channel monitor bus, and the full mixer
  topology (inserts, sends, VCA, groups) comes later on top of the same graph.
  Reviewed by the engine owner, 2026-10-13: accepted with changes, all
  applied in this text.
- Specs: [`session-format.md`](../specs/session-format.md),
  [`timeline-model.md`](../specs/timeline-model.md).

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
  (data written, then the index stored with release). The audio thread loads
  with acquire and reads. A slot found empty is silence and a counted
  underrun, never a wait. The plan's structure is immutable; its slot array
  is its one mutable part, written only by the reader. A chunk shared by the
  playing and the pending plan is linked from both, and the reader alone
  keeps its link count.
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
