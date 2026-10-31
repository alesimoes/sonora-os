# ADR-0024 — Buses, paths and output routing (session format 12, protocol 30)

- Status: **Accepted**, 2026-10-27, by the coordinator on the recommended
  defaults below.
- Date: 2026-10-27
- Proposed by: rt-engineer and ui-engineer, backlog item `ui.mixer.io`
  ("I/O setup and routing: buses, aux tracks, master").
- Extends: ADR-0022 (aux buses, sends, loops refused at the model),
  ADR-0013 (the monitor bus), ADR-0012 §7.3 (PDC over the plan's graph),
  ADR-0009 §3 (structure undoable, values not).
- Numbering: follows ADR-0023. PR #164 (protocol 29) adds no ADR.

## Summary

*Navigation only; the sections below are the decision.*

- §1: the routing model, whole: every audio strip (track, aux bus) has
  one **output**, the Master or an aux bus. The Master is the monitor bus:
  one per session, never deleted, as Logic's Stereo Out. Hardware output
  pairs and named I/O paths are §5, a later slice.
- §2: `output` is an `Id` on `TrackEdit` and `Bus`, 0 for the Master.
  `SetOutput` changes it and is an undo step, like a send's destination.
- §3: sends and outputs together form the bus graph, which stays acyclic:
  `SetOutput` that would close a loop is refused naming the path, as
  `AddSend` is; `RemoveBus` is refused while a strip outputs to it.
- §4: aux solo. `BusMix::solo`, mix state. Soloing an aux solos what
  feeds it, by output or send, through any number of buses (Logic's
  behaviour); aux returns stay solo safe (ADR-0022 §1). Computed on the
  audio thread from the playing plan, so it follows the plan at the swap.
- §5: what is left for later slices, each a BACKLOG line: hardware output
  pairs and the I/O setup page, Master and aux inserts (plugin hosting
  for a strip that is not a track), aux meters (state layout), the
  Master's output pair in the Mix window.
- §6: the plan: a strip's last node feeds its output's `AuxSum` instead
  of `BusSum`; no new node kind, the same node and edge counts. Bus order
  is topological over sends and outputs. PDC is `compensate()` unchanged.
- §7: session format 12, window format 9, protocol 30.

## Context

The gap analysis for `ui.mixer.io` (PR description) found:

- Exists: aux buses (a stereo bus and its return, ADR-0022), sends A-J,
  the monitor bus with a gain, a pan law and an output pair in the model
  (`MonitorBus::outputs`, `SetMonitorBus`), shown as the "Monitor" strip
  with a fader and two meters; PDC over the whole graph.
- Missing: an output assignment for any strip (every track and every aux
  return feeds the monitor bus); aux solo; inserts on an aux or on the
  Master; aux meters; hardware outputs other than the monitor pair; an
  I/O setup page naming paths.

Pro Tools' I/O Setup separates paths (busses, outputs) from the tracks
that use them; Logic hides the bus behind the Aux strip and gives every
strip an output slot. Both let any strip's output be a bus, which is what
makes subgroups (drums to a drum aux, the drum aux to the Master) work.
That is the most used routing in a mix and the smallest step from
ADR-0022's model, so it is this slice.

## Decision

### §1 The routing model

Every audio strip has exactly one output. A track's or an aux return's
output is either the **Master** or an **aux bus**. The Master is the
monitor bus of ADR-0013 under the name the operator knows: it sums every
strip whose output is the Master, applies its gain, and feeds its output
pair (`MonitorBus::outputs`). One per session, never deleted, never
renamed, as Logic's Stereo Out and a Pro Tools session's Master Fader on
the main path. A MIDI track has no audio and no output.

This keeps ADR-0022's one-object aux (the bus and its return) and adds
outputs on top; it does not split the bus into a path and a strip. Pro
Tools' "path with any number of Aux Inputs, or none" is not needed for
subgroups and sends, and §5's paths can be added beside it without
changing what an output names (an id).

### §2 `output` and `SetOutput`

- `TrackEdit::output` and `Bus::output`: an `Id`, 0 for the Master, else
  an aux bus of the session. A MIDI track's is 0.
- `SetOutput { strip, output }`: undoable, edit state. An output is graph
  structure, as a send's destination is (ADR-0022 §3); undoing it puts the
  routing back with one step.
- `DuplicateTrack` copies the output: the copy plays where its source
  does. `RemoveTrack` takes the track's output with it.
- Refused: a strip that is neither a track nor a bus; a MIDI track; an
  output that is not 0 nor an aux bus; a bus routed to itself; a loop
  (§3).

### §3 One graph, kept acyclic

A bus's sends and its output are its edges in the bus graph; a track has
no input, so a track's output can never close a loop. `SetOutput` of bus
A to bus B is refused if B reaches A over sends and outputs, naming the
path ("bus Drums to bus Verb would make a feedback loop: Verb -> Drums
already"), and `AddSend` now also follows outputs. `check_invariants`
refuses a document with a loop over either. `RemoveBus` is refused while
a send targets the bus or a strip outputs to it, naming the strip; the
operator reassigns it first, or the interface sends one compound.

### §4 Aux solo

`BusMix::solo`, saved, never undone, like a track's solo. While any track
or aux is soloed, a track plays only if it is soloed, solo safe, or feeds
a soloed aux by its output or a send, directly or through other buses
(Logic's behaviour; Pro Tools users get it by solo-safing). Aux returns
are never muted by a solo, as ADR-0022 §1 decided, so a soloed aux plays
with everything downstream of it.

The engine computes "feeds a soloed aux" once per callback: one reverse
pass over the playing plan's nodes from each soloed `AuxFader` along input
edges, into a per-node flag array sized at session creation. It is the
graph the audio thread is playing, so a swap that reroutes a track changes
its implicit solo in the same callback, never before or after. No
allocation, no lock.

A bus's solo left on an engine bus slot the document no longer has (an
undone AddBus) would mute the rest while every replica shows none, as a
track's would (ADR-0023 §4); the controller clears the slots past the
document's buses each time it writes the mix.

### §5 Later slices

Each a BACKLOG line under `ui.mixer.io`:

- **Hardware outputs and I/O setup.** Named output paths (Pro Tools'
  I/O Setup) mapping pairs of the device's channels, the UMC1820's 10
  outputs among them; an `output` may then name a path, and the plan gets
  a sink per path aligned with the Master by PDC. Needs the device's
  channel counts on the wire (`Hello`), which no message carries yet.
- **Master and aux inserts.** `PluginHosting` resolves chains by
  `TrackEdit`; a chain on a strip that is not a track needs that keyed by
  strip. With aux inserts, `compensate()` must keep an aux's return line
  while its insert's tail plays after the last send goes (#165 review).
- **Aux and Master meters** in the state block (a layout bump), and the
  Master's output pair chosen in the Mix window.
- **Implicit solo shown** on the strips, as Pro Tools outlines it.

### §6 The plan

`MixLayout` gains `track_outputs` and `bus_outputs`, an engine bus index
per strip or `kNoBus` for the Master. `layout_mix_graph()`:

- A strip's last node (`TrackOut`, `TrackFader` or `AuxFader`) feeds its
  output's `AuxSum`, an edge with `send = kNoSend`, or `BusSum`. Every
  strip still has exactly one such edge, so `mix_graph_size()` is
  unchanged and the session's scratch sizing (ADR-0022 §5) still holds.
- `AuxSum` sums the routed strips first (tracks in display order, then
  buses in plan order), then the sends in ADR-0022 §7's order.
- The builder orders buses topologically over sends and outputs, ties in
  display order, and refuses a plan with a loop. A bus output the layout
  cannot place forward (a hand-built layout) feeds `BusSum` instead.
- PDC: `compensate()` as it is. A routed strip arrives at `AuxSum` aligned
  with the other routed strips and the sends; the aux's return is aligned
  at `BusSum` with the dry paths. An edge's pair changes when its strip is
  rerouted, so its delay line starts afresh: the cut ADR-0022 §6 accepts
  for a send removed at a swap.

### §7 Format, protocol

- Session format **12**: `output` on every track with audio and on every
  bus, `solo` on every bus. A version-11 file migrates with every output
  0 (the Master) and no aux soloed, which is what it played.
- Window format **9**: the undo window's tracks and buses carry `output`;
  the bus mix carries `solo`.
- Protocol **30** (29 is PR #164's): the operation codec has `SetOutput`,
  and `BusMix` carries `solo`; a version-28 peer refuses both.

## Consequences

- Subgroups work: tracks to an aux, the aux to the Master or another aux,
  compensated and loop-free.
- The Mix window's strips get an output selector; the right-hand strip is
  "Master".
- Aux solo is heard and saved; nothing shows implicit solo yet.

## Alternatives rejected

- **The Master as a new `TrackKind` or a new `Bus`**: a second object for
  the one the monitor bus already is, with a migration of every session,
  for no audio gain. A Master that can be deleted is a session that plays
  nothing.
- **Output as mix state** (not undone): an output reshapes the graph as
  a send's destination does, which ADR-0022 §3 made undoable.
- **Implicit solo computed on the control thread** into the track
  atomics: it would lag or lead the plan swap that reroutes a track, so a
  reroute under solo would click in or out one callback off.
- **Pro Tools' solo without implicit upstream solo**: soloing a reverb
  return would play nothing, which is why Pro Tools users solo-safe it.

## Open questions

- Pro Tools mutes an aux's sources when the aux alone is soloed unless
  they are solo safe; if the owner wants that model, §4's reverse pass is
  dropped and nothing else changes.
