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

## Amendment: native.editors (the plumbing the first section left out)

Protocol stays 65; `kTelemetryVersion` stays 1 except one behaviour:
`begin_write()` clears `meter_mask`, so a frame states which meters it
publishes and a bit left from an earlier frame cannot show a stale value as
live; `read()` zeroes a non-finite meter as well as unmasking it, and
`encode(PluginTelemetry)` refuses what `decode` refuses.

**Host.** The engine creates one telemetry region per host process
(`HostProcess`, `create_shared_writable`, `telemetry_init` for the chain's
instance count), passes it as descriptor 6 with `--telemetry-fd 6` and keeps
its own mapping, which only `HostProcess::read_telemetry` reads. A region that
cannot be made, or that the host finds not to be layout 1, leaves the chain
running with no telemetry, said on the host's stderr and shown by the
interface as "waiting"; telemetry is never a reason for a chain not to start.
The host maps it read-write, checks the header, and hands block *n* to the
plugin of chain instance *n*.

**Hand-over to a plugin.** The plugin is loaded by lilv or by the VST3 module
loader, so nothing of ours is between the host and its constructor. Each
Sonora bundle exports one symbol, `sonora_native_telemetry_bind(void*)`
(`lv2.version`, `vst3.version`); right after instantiating, the host calls it
with the block. `native::Telemetry` enrols itself in a small per-library list
when it is made; the call binds the oldest instance made since the last call
and leaves the others private, because a mono plugin run once per channel has
several instances and only one may write the insert's block. The DPF probe
instance, which lives for the whole run, withdraws itself. A third-party
plugin has no such symbol and publishes nothing.

**Declaration.** A processor returns `UiHints` (`ui_hints.h`): per parameter
a section, a widget and the curve it shapes, and its meters and curves, whose
slot is their place in the table. `descriptor_json` writes them into the
bundle's `descriptor.json`; the scan worker reads that file
(`apply_descriptor_hints`, engine/control) into the catalog entry, matching
parameters by the table's id (LV2 port symbol) and by name for VST3, whose key
is a number. All or nothing: a descriptor the decoder would refuse is not
applied.

**Engine loop.** Each control-thread turn after the hosting poll,
`PluginHosting::read_telemetry` collects the latest frame of every insert of
every running host, `telemetry_frame` keeps only the slots the catalog
declared, `TelemetryThrottle` admits at most 30 a second per insert and none
for a sequence already sent, and the frame goes to every client.

**Adoption.** `ProcessorBase::publish_levels` makes `process()` publish input
and output peak (dBFS) once per call, and `on_publish` lets a processor add
its own meters and curves inside the same frame. The compressor publishes its
gain reduction and the static transfer curve; the FET and the opto publish the
gate's and the cell's own gain reduction; the limiter publishes the deepest
reduction of its look-ahead stage and its true-peak guard; the EQ publishes its
magnitude response (128 points, 20 Hz to 20 kHz) recomputed only when a
parameter moved. Not adopted here: the Vari-Mu (not on main), the reverb and
the cabinet (their panels are generic).

**Interface.** The panel groups controls by section, draws each as its plugin
asks, shows values with their unit, and draws the plugin's curves and meters
above the controls. A meter that was not published, or whose frames stopped,
draws no bar and says so. The header has Bypass and A/B compare: B starts as a
copy of A and each toggle plays the other set as one gesture. A preset menu is
not there: the engine's preset store has no interface message, which is a
contract of its own.

## Amendment 2026-11-07: the hand-over is pulled by the plugin, not exported

The nightly's lv2lint refused every bundle: an LV2 binary may export only
`lv2_descriptor`, and `sonora_native_telemetry_bind` was a second global
symbol ("Plugin Symbols"). The hand-over above is replaced; lv2lint is not
relaxed.

The bundles now export nothing but their format's entry points (`lv2.version`
lists `lv2_descriptor`, `vst3.version` the three VST3 ones). The direction is
reversed: `sonora-plugin-host` links with `ENABLE_EXPORTS` and exports
`void* sonora_host_telemetry_take()`. Around instantiating a plugin the host
holds a `TelemetryOffer` with the insert's block (control thread, one at a
time); the DPF glue looks the function up with `dlsym(RTLD_DEFAULT, ...)` at
the end of its constructor, and again after each `activate()` (a processor
that publishes offers its `Telemetry` when it is prepared, which on VST3 can
be after construction), and passes the block to `Telemetry::bind_pending`.
The first take clears the offer. "Oldest pending instance" is unchanged. Any
other host does not export the function, the answer is nullptr and the
telemetry stays private. A standard LV2 `extension_data` was the first
choice but DPF owns that function and offers no hook to add an interface
without patching the vendored sources. No change to the wire or the protocol.
