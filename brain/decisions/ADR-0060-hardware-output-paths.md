# ADR-0060: Hardware output paths and I/O Setup (session format 25, protocol 68)

Status: Accepted (engineer, 2026-11-06; recommended defaults, nothing here needs the owner).
Backlog: `ui.mixer.io`, the line "Hardware output paths and the I/O setup page".
Builds on: ADR-0024 section 5 (this is the slice it left), ADR-0022 (aux buses), ADR-0012 section 7.3 (PDC).

## Summary

An output path is an aux bus that plays on a pair of the device's outputs instead of feeding the Master. `Bus` gains `hardware` (the first output of its pair, even, or none) and one undoable operation, `SetBusPath {bus, name, first}`, names a bus and makes it a path or an ordinary bus again. A strip is routed to a path by the existing `SetOutput`, so loops, solo and the Mix window's output chooser need nothing new. The plan gets one `PathOut` sink per path, after the Master's `HardwareOut`, aligned to the Master by PDC. `Hello` carries the device channels the engine opened (`input_count`, `output_count`), which is what I/O Setup lists. Session format 25, protocol 68.

## Context

ADR-0024 left "named output paths mapping pairs of the device's channels (the UMC1820's 10 outputs), an output naming a path, a sink per path aligned with the Master" for later, and noted that no message carries the device's channel counts. The engine registered two outputs only (out_1, out_2), so a second pair did not exist to play on.

## Decision

1. **A path is a bus.** `Bus::hardware` is `kNoHardwareOut` or the first channel of the pair (counted from 0, even, `first + 1 < kMaxOutputChannels` = 64). A path keeps its sends, fader, pan, mute and solo, because it is the aux bus it was. Its `output` is the Master and is not followed. Anything that routes to a bus (`SetOutput`, sends) routes to a path.
2. **`SetBusPath`.** Undoable, one step. It sets the bus's name and `hardware`. Refused for an odd or out-of-range pair, for a pair another path plays on, and for a pair the Master plays on (`mix.bus.outputs`); `SetMonitorBus` is refused symmetrically. A path's `SetOutput` is refused ("an output path plays on the interface"); becoming a path sets its output to the Master in the same step. The names of the device's channels ("Kick In", "Mon L") are not in the session: they say what is cabled to this machine and are kept in the user's config.
3. **Plan.** `MixLayout::bus_hardware` marks the paths. A path's return feeds a `NodeKind::PathOut` node (one input edge, `PlanNode::hardware_out`) and not the `BusSum`. `PathOut` nodes come after `HardwareOut`. `compensate()` raises the `BusSum` to the slowest path and gives every `PathOut` the Master's latency, so the Master and every path play the same instant; a path's edge takes a delay line from the pool like any edge into a sink. A path whose pair the device does not have is written nowhere and the rest of the mix is unchanged. No Master gain, meter or tap applies to a path.
4. **Audio thread.** Nothing new allocates: the sink is a `memcpy` of two scratch buffers into the block's outputs, bounds-checked against `AudioBlock::output_count`; the plan is sized by `mix_graph_size()`, the scratch pool by `3 * max_buses` and the bounds check refuses a `PathOut` that is not one edge. Changes reach the audio thread through the existing plan swap.
5. **Hello.** `input_count` and `output_count` are appended: the ports the engine registered. `sonora-engine --outputs N` (default 10, the UMC1820's) registers out_1..out_N; the Master stays on 1-2 until the Mix window chooses its pair (its own BACKLOG line).
6. **Session format 25.** A bus may carry `"hardware"`, only when it is a path. A version 24 file opens with no path; a version 24 file that holds the key is refused. The operation codec and the undo window carry `SetBusPath`.
7. **UI.** Setup > I/O Setup lists the inputs and outputs by name, picks each track's input by name (mix state, saved with the session), and has an Output Paths section: new, name, move to a free pair, remove (strips routed to it go back to the Master in the same step). A refusal is held in the window until the next action, not put on the 2.5 s status line.

## Rationale

Reusing the bus keeps every rule (loops, solo, PDC, naming, undo, the output chooser) in code that ADR-0022 and ADR-0024 already test. A separate "path" object would have needed its own ids, operations, graph kind for strips and chooser entries for what is a bus with a destination.

## Consequences

- One field, one operation, one node kind; format and protocol bump. FakeEngine reports 18 inputs and 10 outputs.
- The Master's own pair is still outputs 1-2 (BACKLOG: Master pair in the Mix window).
- A path does not carry the Master's trim or meter; a metered path is the "Aux and Master meters" line.
- Unverified: the delay-line pool is shared with the Master's edges, so a session with every path and many chains can run the pool dry sooner; `undelayed_edges` reports it as for any edge.
