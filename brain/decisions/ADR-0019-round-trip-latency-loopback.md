# ADR-0019 — Round-trip latency: measured by a loopback probe, compensated at capture

- Status: **Accepted**, 2026-10-25, on the recommended defaults below, by
  the coordinator under the owner's standing authorization of 2026-10-21.
- Date: 2026-10-25
- Proposed by: rt-engineer (Track 1), backlog item
  `engine.playback.input_monitor` ("Input monitoring with latency
  compensation, measured by a loopback test").
- Amends: `brain/specs/timeline-model.md`, "Punch in", the latency term
  (where the capture window sits on the playhead). Fills in ADR-0016 §2's
  deferred L. Changes no contract: neither `contracts/backend` nor the
  wire protocol.
- Numbering: follows ADR-0018.

## Summary

*Navigation only; the sections below are the decision.*

- §1: L, the round trip from a sample the callback writes to the sample the
  callback reads back, is measured by an impulse probe that runs inside
  `Session::process()`, through SPSC rings, out one output and in one input.
- §2: a take's capture window is in timeline frames; the audio thread opens
  and closes it L frames later on the playhead, and labels the file with
  the timeline frame its first sample lines up with. Loop takes keep
  ADR-0016's own L arithmetic.
- §3: input monitoring is not delayed or advanced by L. Monitored input
  goes through the same plan as playback, so it reaches the bus delayed by
  exactly the plan's PDC compensation, as playback does.
- §4: L is a per-machine figure: given or measured at engine start, held by
  the session, refused while a take runs. The drivers fake a loopback cable
  so CI measures it without hardware.

## Context

`timeline-model.md` defines the latency term: a take is shifted earlier on
the timeline by L so what was played lines up with what was heard; its
TimeReference is `P0 − L`. ADR-0016 counts a loop take's wraps with the
same L. Both said "until the loopback measurement lands, L is the backend's
reported latency", and `contracts/backend` reports none, so L was 0
everywhere: on the target UMC1820 at 128 frames a take landed about 6 ms
late.

JACK's port latencies would give a figure without a cable, but they do not
include the converters' group delay, which on USB interfaces is a large
and model-specific part of the round trip. A loopback measures all of it.

## Decision

### §1 The probe

`LatencyProbe` (engine/core) is owned by the session and runs at the end of
every callback, after the mix. Armed from the control thread with an output
index, an input index, a number of pings, the longest round trip looked
for (W, default a quarter second) and a level (default −20 dBFS), it plays,
per ping, W frames of silence (the noise floor, and the previous ping's
tail dying out) and then one impulse, and listens W frames for the input's
largest absolute sample. Its offset is that ping's round trip. The result:

- measured, the median of the pings, when every ping's peak is at least
  −60 dBFS and 12 dB over the floor and the pings agree within one frame;
- no signal, unstable (the pings disagree), a discontinuity (the backend's
  frame time jumped more often than the pings allow restarts for), no such
  port, or cancelled, each said with its figures.

Requests and results cross by `RingBuffer`; the audio thread never waits,
allocates or reads a clock. The probe overwrites its output channel only
while it runs; the operator cables a spare output to a spare input. The
largest sample, not a threshold crossing, is the arrival: it is
independent of the loop's gain and polarity (|x|), and on a converter's
symmetric filter it is the filter's centre, which is the delay a recording
is shifted by.

### §2 Where L applies

The audio thread latches L at a take's first block. For a plain or punched
take, capture over `[capture_from, capture_to)` in timeline frames runs
over `[capture_from + L, capture_to + L)` on the playhead, and the Open
record's timeline is the playhead less L. So the file's TimeReference is
`P0 − L` (session-format.md), a punch file covers exactly the timeline
range its handles ask for, and `add_closed_take()` places it with no
latency arithmetic of its own: `source_offset = A − (P0 − L)`, the spec's
term. A take started at a playhead below L skips its first `L − P0`
samples, which line up with no timeline frame. `follow_punch_out()` waits
the extra L frames, so the transport records L frames past `B + H`.
`take_origin()` and `ClosedTake::origin` stay timeline frames.

The source's `latency_compensation` records L, and `ClosedTake::latency`
carries it to `add_closed_take()`.

A loop take records from its start with no shift and counts each wrap at
`capture count + L` (ADR-0016 §2, C7); L is now the session's figure
instead of 0.

### §3 Monitoring

Monitored input cannot be compensated: it is late by L the moment it is
heard. The engine adds nothing to it beyond what the plan adds to every
path into the bus (PDC, ADR-0012 §7.3), so input and playback stay aligned
with each other on the bus. Auto monitoring still switches on the playhead
at A and B (timeline-model.md); the input it switches to is the
performer's playing L frames after they heard that frame.

### §4 Where L comes from

L belongs to the machine (interface, period, cable), not to a session file.
`sonora-engine --input-latency FRAMES` sets it; `--measure-latency OUT:IN`
measures it on `out_OUT` and `in_IN` once inputs are connected, before the
control server starts, and says what it found. A failed measurement is
said and leaves L at the given figure or 0. `Session::set_input_latency()`
refuses during a take and above one second.

`FakeBackend::Config::loopbacks` cables an output to a capture port a set
number of frames later, so `engine/core` and `engine/control` measure and
check L in CI.

## Consequences

- Takes on a measured machine land where they were played. With L = 0 the
  behaviour is unchanged.
- A punch's capture ends L frames later on the playhead: a post-roll
  shorter than `H + L` past B still records the whole handle.
- Not here: a probe command on the wire (a protocol change, and the
  interface's latency page), persisting L per machine, and the backend's
  reported figure as a fallback. Each is a backlog item.
