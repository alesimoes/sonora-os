# ADR-0057 — Plugin telemetry region and descriptor hints

Status: accepted. Amends ADR-0011 (native suite, M9 telemetry), ADR-0012
(hosting, a third shared region) and ADR-0007 (contract list).

## Summary

- A plugin shows its editor more than parameters: gain reduction, input and
  output level, a response curve. Today nothing carries them, so the generic
  editor has no meters (BACKLOG N1 "with M9").
- A third shared-memory region, the TELEMETRY region
  (`contracts/plugin/telemetry.h`, layout 1), one block per insert, written by
  the plugin's audio thread through a seqlock of atomics: up to 8 scalar
  meters and up to 2 curves of at most 128 points. Wait-free, no allocation,
  no lock. The channel region and `kLayoutVersion` are untouched.
- The engine's control thread reads it (never the audio thread, never
  waiting: a torn read is retried a bounded number of times and otherwise the
  last frame stays) and publishes `PluginTelemetry` (wire type 10) to the UI at
  no more than `kTelemetryMaxHz` = 30 per insert, nothing for a frame the
  client already has.
- The catalog says what a plugin publishes: `PluginInfo::meters` and
  `curves` (id, name, unit, kind, range), and each `PluginParam` gains
  optional `section`, `widget` (Auto, Knob, Fader, Switch, Choice) and
  `curve_source`. Absent is today's behaviour.
- Protocol 65. As for every bump, a peer of another version is refused at
  Hello; there is no half-read catalog.
- SDK helper `native::Telemetry` (`plugins/native/sdk`). No editor and no
  per-plugin adoption here: that is `native.editors`.

## Context

`ProcessorBase::bounded_blocks()` already says "the M9 telemetry region
publishes this to the editor". The compressors compute gain reduction, the EQ
computes its response, the reverb and tube suites compute per-instance
contributions that ADR-0011 3.3 says an editor must show ("do not stack
silently"). All of that dies inside the plugin host process. A plugin runs
out of process (ADR-0012), so the path is shared memory to the engine, then the
control wire to the UI.

## Decision

### 1. The region

One region per host process, created by the engine, laid out as a 32-byte
`TelemetryHeader` (magic, version, sizes) and `kMaxInstances` (16) cache-line
aligned `TelemetryBlock`s; the size is a constant so the host is told nothing.
The engine hands the host a read-write descriptor and keeps its own `PROT_READ`
mapping of the same memfd, as for the clock region. It is a region of its own,
not new fields in `ChannelHeader`: that header's version gates the audio
protocol, and a telemetry change must not invalidate it.

A block holds `seq`, a meter mask, a rejected counter, up to 8 `float` meters
and up to 2 curves of up to 128 `float`s (about 1.1 KB). Every word is an
atomic, so a read that overlaps a write is not a data race.

Writer (plugin audio thread), single writer per block: `begin_write()` makes
`seq` odd, `set_meter`/`set_curve` store relaxed, `end_write()` makes `seq`
even with release. A non-finite value is written as 0 and counted. The calls
carry `SONORA_RT`, so the RTSan build aborts on any allocation, lock or
syscall in them; `test_plugin_telemetry` and `test_native_telemetry` also
count `operator new` across thousands of frames.

Reader (engine control thread): acquire `seq`, copy, acquire fence, re-read
`seq`; up to 8 attempts, then it keeps the last frame. It is never on the audio thread.

Trust: the region is writable by third-party code (protocol.h, "Trust"). The
reader checks magic, version and sizes, never turns a word into an offset,
clamps counts before indexing (a curve count over 128 refuses the frame), masks
meter bits to the 8 that exist and drops a non-finite meter's bit. The
descriptor, not the region, says what a slot means; a plugin cannot add a meter
the catalog did not declare, because the engine forwards only declared slots.

### 2. Descriptor extension (protocol 65)

`PluginParam`: `section` (at most 64 bytes), `widget` (`PluginWidget`: Auto = 0
is every parameter before 65; Switch needs 2 steps, Choice at least 2), and
`curve_source`, the id of a published curve the parameter shapes (an EQ band
on the response); it must name a curve of the same plugin. `PluginInfo`:
`meters` (dense slot, unique id, name, unit, `PluginMeterKind`, range) and
`curves` (dense slot, unique id, axes with log or linear x, 2 to 128 points).
The decoder refuses what would mislead an editor (a gap in the slots, an
inverted range, a log axis from 0, an unknown widget, a dangling
`curve_source`) with a message that names the plugin and the entry.

Old-client compatibility is the version gate, not tolerance of unknown bytes:
the wire is strict (`Reader::done()`), as for the 42 previous bumps. A
version 42 client meets a version 65 engine at Hello and reads the mismatch
message naming both. What stays compatible is the meaning: every field is
optional, and a catalog that sets none of them behaves exactly as before.

### 3. The message

`PluginTelemetry { insert, sequence, meter_mask, meters[], curves[][] }`
carries values only; the catalog carries the meaning. The mask says which
meters have been published. The engine side uses `TelemetryThrottle`
(`contracts/api/telemetry_throttle.h`): at most 30 per second per insert and
none for a repeated sequence, so a curve costs about 15 KB/s. A frame that
decodes with a non-finite value, a mask past the meters sent or over-cap counts
is refused whole.

### 4. SDK

`native::Telemetry` (`begin`, `meter`, `curve`, `end`) writes to a private
block until a host `bind`s the shared one, so `process()` never branches on
whether anyone is listening and the measurement harness reads the same values.
Adoption (compressors' gain reduction, the EQ response, `bounded_blocks`) is
`native.editors`. The SDK takes the header by include path, not by the
`sonora_plugin_protocol` target, because the neutral (MSVC) build does not
configure `contracts/`; this is a proposed edge in ADR-0011 2.2's table.

## Not decided here

- The host side of the region (creating the memfd, mapping it in
  `sonora-plugin-host`, binding blocks per instance) and the engine's
  control-thread read loop. They need the process-level plumbing and belong
  with `native.editors`, which is the first user. Until then the fake engine
  (`FakeEngine::script_plugin_telemetry`) is the only publisher.
- Third-party plugins publish nothing; their meters stay the output meter.
- A third curve or more meters: the layout moves to version 2.

## Consequences

- `kProtocolVersion` 65; `kTelemetryVersion` 1; `kLayoutVersion` unchanged.
- A catalog is a little larger (two lists per plugin, three fields per
  parameter).
- Tests: `plugin_telemetry` (layout, round trip, torn reads from a second
  thread, scribbled words, no allocation), `native_telemetry` (the SDK helper,
  no allocation), `api_wire` (round trips, each refusal, absent hints, the
  throttle) and `api_conformance_fake` (a scripted frame through the codec).
