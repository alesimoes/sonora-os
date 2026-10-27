# ADR-0022 — Sends, pre/post fader, onto aux buses (session format 11, protocol 27)

- Status: **Accepted**, 2026-10-27, by the coordinator on the recommended
  defaults below.
- Date: 2026-10-27
- Proposed by: rt-engineer and ui-engineer, backlog item `ui.mixer.sends`
  ("Sends, pre/post fader").
- Extends: ADR-0012 §7 (node kinds, PDC over the plan's graph), ADR-0013
  (the monitor bus as the one output), ADR-0009 §3 (structure undoable,
  values not).
- Leaves to `ui.mixer.io`: buses as I/O paths apart from their returns,
  aux inserts, aux output routing, aux solo, aux meters.
- Numbering: follows ADR-0021.

## Summary

*Navigation only; the sections below are the decision.*

- §1: a send needs somewhere to go and the session had one bus, the
  monitor bus. This ADR adds the least that makes sends real: an **aux
  bus**, a stereo bus and its return strip as one object, Logic's "Bus N"
  with its Aux strip, Pro Tools' internal bus with the one Aux Input that
  takes it. The return feeds the monitor bus.
- §2: **10 send slots per strip, A to J**, as Pro Tools; tracks and aux
  buses both have them.
- §3: structure (bus exists, send exists, its slot and its destination) is
  undoable edit state; values (bus fader, pan, mute; send level, pan,
  follow-main-pan, mute, pre/post) are mix state, saved and never undone,
  as a fader.
- §4: a send that would close a loop through the buses is refused by the
  model, and an acyclic bus graph is a document invariant. A strip sends to
  a given bus at most once.
- §5: in the plan a track with a send is split, its pre-fader point a node;
  a send is an edge from that point into an `AuxSum`, with its own gain
  ramp; `AuxFader` feeds `BusSum`. PDC is ADR-0012 §7.3 unchanged, run over
  the larger graph.
- §6: the send's coefficient: pre-fader is post-insert, pre-fader,
  pre-pan, and silenced by the track's mute (Pro Tools' default); post
  adds the fader. Pan follows the main pan unless the send's own is set.
  Ramped per callback like a fader, from silence when the send is new.
- §7: summing order is fixed: tracks in display order, then aux buses in
  topological order, slots A to J within a strip.
- §8: session format 11 and protocol 27; the engine sizes `max_buses`
  (default 32) at session creation.

## Context

`ui.mixer.sends` asks for per-track sends, pre or post fader, each with a
destination bus, level, pan, mute, sample-accurate and smoothed, summed in
a fixed order, compensated by PDC, loop-free and undoable. The gap
analysis found none of it, and nothing for a send to reach:

- `common/model`: `MixState` has one `MonitorBus`; no bus, aux or send
  type. `TrackEdit` holds inserts only.
- `engine/core`: the plan's graph is per track `TrackOut` or `TrackSource
  → InsertChain → TrackFader`, then one `BusSum` and `HardwareOut`
  (`layout_track_graph`). `compensate()` is generic over the graph.
- `engine/control`: `build_plan` lays out that graph; `apply_mix` writes
  the `Track` atomics.
- Contract, FakeEngine, conformance: operations cross as op-codec bytes;
  no send or bus operation exists.
- Qt: the Mix window's strips have inserts, pan, fader, solo, mute; no
  send slot and no aux strip.

`ui.mixer.io` (I/O setup: buses, aux tracks, master) is a separate item.
Waiting for it leaves this item with nothing to deliver; doing it here
doubles the PR. §1 takes the part of it that sends cannot do without.

## Decision

### §1 The aux bus: a bus and its return, one object

`EditState::buses`, in display order, holds `Bus { id, name, sends }`.
Every bus is stereo and has exactly one return strip, whose values are a
`BusMix { bus, gain_db, pan, mute }` in `MixState::buses`. The return feeds
the monitor bus (`BusSum`), after which the monitor bus gain applies as
before.

This is Logic's model, where choosing a bus on a send makes the Aux strip
that returns it, and the common case of Pro Tools', where an internal bus
is returned by one Aux Input. It is not Pro Tools' I/O Setup, where a bus
is a path that any number of Aux Inputs, or none, take as input; that, a
return's inserts, its output assignment and its solo belong to
`ui.mixer.io`, which can split `Bus` into a path and a strip without
changing what a send names (a bus id).

An aux return is never muted by another strip's solo, as if solo safe:
soloing a track that feeds a reverb plays its reverb, which is what Logic
does and what Pro Tools users set solo safe on their effect returns for.

### §2 Ten send slots, A to J

`kSendSlots = 10`, on tracks and on aux buses, as Pro Tools' sends A–E and
F–J. Logic has 8 per strip; 10 covers it. A slot is unique on its strip;
gaps are allowed, as with inserts. The engine sizes its send state for
`(engine tracks + max_buses) × 10`.

### §3 What is undone

- Undoable: `AddBus`, `RemoveBus`, `AddSend`, `RemoveSend`. They change
  `EditState`. A send is `Send { id, slot, bus }` on its strip.
- Not undoable: `SetBusMix`, `SetSendMix`. A `SendMix { send, level_db,
  pan, follow_pan, mute, pre }` and a `BusMix` live in `MixState`, keyed by
  id, and outlive an undone add, so redo brings the send back where its
  level was. `AddBus` and `AddSend` carry their initial values, as
  `AddTrack` and `AddInsert` do.
- `RemoveBus` is refused while a send targets the bus: the operator
  removes those sends first, or the interface sends one compound. The
  bus's own sends go with it.
- A send's destination does not change in place: a reassignment is a
  compound of `RemoveSend` and `AddSend`, one undo step.

### §4 Loops are refused at the model

A send from a bus to a bus makes an edge between them; the bus graph must
be acyclic. `AddSend` from bus A to bus B is refused if B reaches A
(including A itself), naming the path. `check_invariants` refuses a
document with a cycle, so a session file cannot bring one in. The plan
builder still orders the buses itself and refuses a plan with a cycle,
rather than trusting the model.

A strip sends to a given bus at most once. Two sends of one strip to one
bus would be two edges with the same pair of stable ids, which ADR-0012
§7.3's delay-line pool keys on; Pro Tools allows it, and nothing is lost
by asking for one send at the sum of the two levels.

### §5 The plan: split tracks, send edges, `AuxSum` and `AuxFader`

`NodeKind` appends `AuxSum = 6` and `AuxFader = 7`. `PlanEdge` gains
`send`, the index of the send's state (`strip × kSendSlots + slot`;
`kNoSend` for every other edge).

- A track with a send is split even without an insert: `TrackSource →
  TrackFader`, or `TrackSource → InsertChain → TrackFader`. Its pre-fader
  point is the last node before `TrackFader`; every send of the track is
  an edge from there.
- A bus is `AuxSum → AuxFader → BusSum`. `AuxSum`'s inputs are the send
  edges to it; its sends leave from it.
- Node order: tracks in display order, then buses in topological order of
  the bus graph, ties in display order, then `BusSum` and `HardwareOut`.
  Every edge goes forward.
- Stable ids: `AuxSum` and `AuxFader` derive from a bus key kept for the
  session (`PlanContinuity::bus_keys`), as a track's from its track key.
- PDC: `compensate()` as it is. A send from an insert chain leaves after
  the chain's latency, so the edges into `AuxSum` are aligned among
  themselves and `AuxFader → BusSum` is aligned with the dry paths. A wet
  path is never early against the dry, and a reverb on an aux is
  compensated like any insert once aux inserts land.
- The session sizes scratch buffers for `3 × tracks + 2 × max_buses + 2`
  nodes. A plan with more buses than `max_buses` is refused, naming the
  limit, and the engine refuses an `AddBus` past it.

### §6 The coefficient, and its smoothing

For a send of a track:

- pre-fader: `level × pan × (track muted or solo-muted ? 0 : 1)`; the
  signal is post-insert, pre-fader, pre-pan, as Pro Tools' pre-fader send.
  A muted track's pre-fader sends are silent, Pro Tools' default.
- post-fader: the same times the track's gain.
- pan: the send's own, or the track's when `follow_pan` is set (Pro Tools'
  FMP, on by default). The track's pan law for a mono track, balance for
  a stereo one.
- a send's own mute silences it.

For a send of an aux bus the bus's gain and mute stand for the track's.
The aux return is `gain × (mute ? 0 : 1) × balance(pan)`.

Each coefficient is a target read once per callback and ramped linearly
across it from the last callback's value (ADR-0009 §4.5), sample by sample,
so a level step is never a step in the signal. A send or bus that was not
in the last callback's plan starts its ramp from silence. Splitting a
callback into `max_block_frames` pieces does not change the ramp.

A send removed from the plan stops at the swap; ADR-0012's Consequences
accept that cut for a chain edit, and the same is accepted here.

### §7 Summing order

`AuxSum` sums its edges in the order the builder writes them: source
strips in node order (tracks in display order, then buses in topological
order), slots A to J. `BusSum` sums tracks in display order, then buses in
node order. Floating-point sums are therefore the same on every run of the
same document.

### §8 Format, protocol, limits

- Session format **11**: `buses` at the root (`id`, `name`, `sends`), and
  `sends` on each track; `mix.buses` and `mix.sends`. A version-10 file
  migrates with none. The undo window's codec carries the same.
- Protocol **27** (26 is held by a PR in flight): the operation codec
  decodes the six new operations; a version-25 peer refuses them.
- No new `ParamId`: a send or aux fader ride is a `SetSendMix` or
  `SetBusMix` per step, as the monitor bus rides today (mix_model.h).

## Consequences

- Sends and aux returns are heard, compensated and saved; the Mix window
  shows send slots and aux strips.
- `ui.mixer.io` inherits `Bus` and the aux strip, and adds paths, inserts,
  output routing, solo and meters on top.
- The state block is unchanged: aux strips have no meter yet.

## Alternatives rejected

- **Aux returns as a `TrackKind`**: reuses the track strip, but every
  track carries playlists and every edit operation assumes them; one more
  kind touches all of them for no audio gain.
- **A send as a node with its own scratch buffer**: a buffer per send at
  `(tracks + buses) × 10` is megabytes for nothing an edge with a gain
  cannot do.
- **Pre-fader sends unaffected by mute**: Pro Tools' default mutes them,
  and a cue mix that keeps a muted track is the surprise the operator does
  not expect.

## Open questions

- Pro Tools allows two sends of one strip to one bus; §4 refuses it. If an
  owner workflow needs it, the delay-line pair must key on the send.
