# ADR-0009 — Timeline as immutable snapshots, operations as undo and replication, residency-gated plan swap

- Status: Proposed
- Date: 2026-10-12
- Decided by: architect. Rests on one owner decision, 2026-10-12: Phase 2
  playback starts with a fixed 2-channel monitor bus, and the full mixer
  topology (inserts, sends, VCA, groups) comes later on top of the same graph.
- Specs: [`session-format.md`](../specs/session-format.md),
  [`timeline-model.md`](../specs/timeline-model.md).

## Context

Phase 2's exit criterion is: record, play back, punch in over an existing
take, and undo it, **without stopping the transport**. ADR-0007 already fixes
the frame:

- `common/model` holds the session document and is never on the audio thread
  (§1, §2).
- Structural changes reach the audio thread as an immutable plan, published
  through an atomic pointer and freed off the audio thread by epoch (§2).
- Undo operations double as the replication format for the UI replica (M8).
- ADR-0008 (portability, accepted by the owner 2026-10-12, on its own branch
  at the time of writing) makes `common/model` platform-neutral.

What those leave open, and what this ADR decides, are the choices that
cost a rewrite if they turn out wrong: how undo is represented, which state
it covers, whether regions may overlap, when a plan may be published given
that its audio may not be in memory, and the on-disk encoding.

## Decision

### 1. The edit state is an immutable value; undo is a pointer

The undoable part of the session (`EditState`: tracks, playlists, regions,
fades, crossfades, markers) is never mutated. An edit produces a new
`EditState` that shares every unchanged playlist and track with the previous
one. The undo stack holds those values. Undo and redo move a pointer.
The stack is bounded (200 steps by default). Retired values are freed on
the document thread.

### 2. Changes are resolved operations, applied by a pure function

Every change is an `Operation` value carrying every id and number its result
depends on. `apply(Snapshot, Operation) -> Snapshot` reads no clock, allocates
no id and touches no file. The engine resolves commands into operations on
one document thread in `engine/control`, applies them, and sends each, with
the revision it produced, to every replica. Replicas apply the same function
and never apply their own. Undo and redo are sent as messages that move the
replica's own pointer. On a revision gap the replica resyncs from a full
snapshot in the session file's encoding.

This is ADR-0007 M8's "undo operations are the replication format", made
concrete: what replicates is the forward operation; undo needs no inverse
operation at all.

### 3. Sources and mix state are outside undo, and playlists do not overlap

- **Sources are an append-only pool.** Undoing a take removes its region,
  never its source or its file. Removing unused audio is an explicit,
  confirmed operator action.
- **Mix state** (gain, pan, mute, solo, the monitor bus) and session settings
  are saved but not undoable. At run time the mix values are the `Track`
  atomics they already are.
- **A playlist's regions never overlap.** Placing a region over others trims
  and splits them in the same operation. A crossfade is an explicit object
  between two butting regions, using each one's handle. At most two regions
  play on a track at any instant.

### 4. A plan is published only when its audio is resident

Refines ADR-0007 §2 without changing it.

- Playback reads through a **source chunk cache**: fixed-size chunks of
  source files, in a pool allocated and locked at session creation, looked up
  by the audio thread in a fixed-size lock-free table. A missing chunk is
  silence and a counted underrun, never a wait.
- The document thread builds the plan, a **separate disk reader thread**
  loads the chunks it needs from the current playhead through one read-ahead
  window, and only then is the plan pointer swapped. Until then the previous
  plan keeps playing. Locate uses the same gate: load, then send the locate
  `RtCommand`.
- Chunk buffers are reclaimed by the plan's epoch rule: unlinked first,
  reused after the audio thread has begun a later block.
- The disk reader and the capture writer are separate threads. A slow read
  never delays a capture drain.
- Fades, crossfades and region gain are applied on the audio thread from
  the plan, at most two voices per track.
- The plan is a node list in topological order (`TrackOut` → `BusSum` →
  `HardwareOut`), with a per-path delay field that is zero in Phase 2. Phase 2
  runs it serially. The Phase 3 mixer adds node kinds, and the Phase 4
  parallel scheduler and PDC run the same list.

### 5. The session file is canonical, strict JSON; one parser for the project

The format and its rules are in `session-format.md`: integer samples for
every position, versioned with a single integer, migrated forward, refused
when newer, atomically saved through a `common/platform` durability seam.

The session file needs integer-exact numbers, which the key map's
parser, storing every number as `double`, does not provide. Rather than a
second private parser, a strict JSON reader and writer moves into a new
platform-neutral module, **`common/json`** (target `sonora_json`, layer 0
beside `common/base`, standard library only, owner rt-engineer, ui-engineer
reviews). It keeps the integer lexeme, reports line and key path, and caps
size and depth. `common/model` uses it from the start; `ui/keymap` moves onto
it in its own PR. This adds a module and edges to ADR-0007 §1's graph, which
is an additive change under ADR-0007 §6: `common/json` may be linked by any
module except `common/base` and `contracts/*`.

## Rationale

- **The worst outcome decides undo.** A command-pattern undo stores an
  inverse for every operation, written by hand. Each inverse is a chance to
  restore something subtly different, and the overwrite operation that
  punch-in uses touches up to four regions and their crossfades. A bug there
  is a take that is gone after undo. With snapshots, undo cannot restore the
  wrong thing, because it restores the thing itself. The price is memory,
  bounded by structural sharing and the stack limit, and measured before
  M8 lands.
- **Undo during a take, safely.** The take in progress is not in the
  document until punch-out, and the document has one writer. An undo pressed
  while recording changes prior edits. The take then commits on top of the
  result. No race needs reasoning about.
- **Non-overlap is an RT bound and an AAF shape.** Layered regions make the
  number of voices per track unbounded and push layer resolution into the
  callback. Non-overlapping playlists with explicit crossfades are a
  `Sequence` with `Transition`s, which is what AAF stores. So the Phase 4
  round trip is a mapping, not a flattening.
- **Residency gating is what "without stopping the transport" costs.**
  Every other piece (atomic plan swap, epoch retirement) is already decided.
  Without the gate, an undo during playback swaps in a plan whose audio was
  evicted, and the operator hears a hole exactly where they are listening.
  With it, the operator hears the old edit for a few tens of milliseconds
  longer, which nobody can perceive.
- **Fail loudly.** A session newer than the program is refused. A missing
  file is listed on screen at open. A crossfade without handle material is
  refused rather than shortened. A missing chunk is counted, not hidden.
- **Platform neutrality is kept by construction.** `apply` is pure and the
  model does no I/O. Durability goes through `common/platform` (ADR-0008).

## Consequences

- **`engine/core` gains a second disk thread and a cache.** The disk writer
  stays as it is. The reader, the chunk table and the pool are new RT-adjacent
  code: the table lookup is `SONORA_RT` and gated by `rt-safety-auditor`.
- **Memory.** Cache pool of roughly 55 MB at 18 tracks with an 8 s window,
  locked. Undo history is bounded by measurement.
- **The engine must close a take without stopping the transport, and the
  take-epoch race must be fixed first.** Punch-in turns the "implausible"
  stall in `planning/BACKLOG.md` into an operator-speed cycle. That fix,
  with its regression test, is now on Phase 2's critical path.
- **Two new modules**: `common/json` (§5) and `common/model` (ADR-0007 M8).
  `common/platform` is created by whichever PR first needs it (ADR-0008); the
  session writer's durable save is a candidate.
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

1. **Mixed disk throughput on a USB stick** (the cache's window and chunk
   size). 18 tracks recording 24-bit/48 kHz while 18 play back, on the
   persistence partition of a physical stick. Chunk sizes 64 k and 256 k
   frames, worst-case read latency. Researcher; result in `brain/research/`.
   Until it exists, 8 s and 64 k frames are placeholders.
2. **Durability of `rename` plus directory `fsync` through the persistence
   overlay** (`home/rw` on ext4). Kill a QEMU VM at random points during a
   save loop, 200 times; the file must always parse and be the previous or the
   next save. Researcher.
3. **Undo memory.** 18 tracks × 2,000 regions, 200 single-region edits on the
   longest playlist, peak RSS. Over about 64 MB means chunked playlists.
   rt-engineer, in the M8 PR.
4. **Plan build time** for that session. Over about 20 ms moves the builder
   off the document thread. rt-engineer, in the M8 PR.
5. **AAF fade representation**: whether libaaf reads and writes a fade on a
   lone clip through `SourceClip`'s fade properties, and what other DAWs
   emit. Researcher, before Phase 4.

## Alternatives rejected

**Command pattern with hand-written inverses, over a mutable model.** It is
the textbook design and the lightest on memory. Rejected because every
inverse is a place for undo to restore the wrong thing, and the operations
Phase 2 needs most (overwrite on punch-in) are the hardest to invert. It also
needs a lock or a copy whenever a save or a plan build reads a model being
edited. Its memory advantage is real but bounded here by sharing and a stack
limit.

**Full copy of the session per edit.** Simple and correct. Rejected on cost:
proportional to the session, not to the edit, and a 36,000-region session
would copy megabytes per nudge.

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

**A second private JSON parser in `common/model`.** No new module.
Rejected because two strict parsers drift into two dialects, and the key
map's comment already names session files as the point to revisit.
