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
  wire protocol. The update of 2026-10-26 changes both (protocol 22).
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
- Update 2026-10-26: the probe on the wire, L kept per machine, the
  backend's figure as a fallback.

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
  reported figure as a fallback. Each is a backlog item; all three are the
  update of 2026-10-26.

## Update, 2026-10-26: the probe on the wire, L kept per machine, the backend's figure

Taken on the recommended defaults (`t1-latency-wire`). Protocol 22: main is
at 20 (untitled sessions), and 21 is loop record's in flight; whichever
merges later renumbers. The engine's `--latency-file PATH` names the
machine file (tests point it into their own folder; "" keeps nothing).

- **Commands.** `latency.measure` takes `OUT:IN`, the port numbers from 1 as
  `--measure-latency` takes them, and arms the probe; `latency.cancel`
  stops it; `latency.set` takes a frame count and makes it L by hand. The
  control thread handles all three and pumps the probe every turn of its
  loop. Measuring and setting L are refused during a take, a take asked for
  included, as `set_input_latency()` is; a second measurement is refused
  while one runs.
- **One standing event, `EventKind::Latency`.** `frame` is L in effect,
  `flags` its source (none, measured, manual, backend) and the probe's
  status (idle, measuring, or the outcome: measured or one of the five
  failures), `insert` how many outputs the probe may play on, `text` the
  probe's own words with its figures. Sent at start, at every ping while
  measuring and at every change, replaced each time, and sent to a client
  that attaches later, so a late interface sees L. A failed measurement
  leaves L and its source as they were.
- **L is kept per machine, never in the session file** (no session format
  change). One line per key in `latency.conf` in the user's config folder:
  `$XDG_CONFIG_HOME/sonora`, else `$HOME/.config/sonora`, on POSIX
  (`platform::user_config_dir()`, beside `user_cache_dir()`); on the
  Windows port `%APPDATA%\Sonora`, roaming, since L is a setting and not
  regenerable. On the appliance that is the persistence partition's home
  (ADR-0005). The key is the capture device as the server names it (the
  UMC1820 on the appliance), the rate and the period, because the round
  trip changes with each. A measured or manual L is written there, by
  temporary file and rename, from the control thread; `--input-latency`
  is for the run only and is not written.
- **Where L comes from at start**, first that applies: `--input-latency`
  (manual), `--measure-latency` (measured, and kept), the machine file
  (its recorded source), the backend's reported round trip (backend),
  else 0 (none). `AudioBackend::reported_latency()` is the server's
  capture plus playback latency for the session's ports; drivers/jack
  reads JACK's port latency ranges once an input and an output are
  connected, and says 0 otherwise. It misses the converters' group delay
  (Context), so it is a fallback, shown as such. `capture_device()` joins
  the contract for the key. Both have defaults (0, empty), so a backend
  that cannot tell says nothing.
- **The interface's Latency window** (Setup > Latency…): the port pickers,
  Measure and Cancel, the result in the probe's words, the source, and a
  manual entry. A refusal is held in the window until the next action, not
  only on the status line.
