# ADR-0144 - .vstpreset Info names, and what the scan-worker item leaves open

- Status: Accepted
- Date: 2026-11-09
- Item: `plugins.host.scan_worker` (the open #278 lines on the scan worker,
  the catalog cache and the probe record, all on main since PR #95)
- Relates to: ADR-0029 (presets; §5 interchange limits, slice 5 preset
  load), ADR-0012 (host, scan and probe), ADR-0004 (published formats only)
- Spec: `brain/specs/plugin-factory-presets.md`
- No contract, protocol or format change: the probe record stays version 3,
  `kProtocolVersion` is unchanged.

## Context

The scan worker lists a VST3 plugin's `.vstpreset` files in its probe record
(`presets`, `{id, name}`). Until now the name was the file name, and the
file's `Info` chunk, the XML metadata Steinberg's published `PresetFile`
writes (`<MetaInfo>` of `<Attr id="..." value="..."/>` entries), was skipped.
A vendor's factory set often names its files by number or by a slug while
the `Info` chunk carries the name the vendor meant the operator to read.

`Info` is untrusted input: anyone can drop a file into `~/.vst3/presets`, and
the listing runs for every plugin at every scan. XML read with a general
parser brings entity expansion (the "billion laughs" bomb), external
references and unbounded nesting into a process that only wants one string.

Three other lines of #278 touch the same code and are not closed here; this
ADR records why, so they are not taken for forgotten.

## Decision

1. **The `Info` chunk is read for the display name only.** `vstpreset::list`
   reads the header and the chunk list, then the one `Info` chunk (never the
   whole file), and takes the value of `<Attr id="Name">` through
   `vstpreset::info_name`: a bounded forward scan, not an XML parser. Caps:
   the chunk at 64 KiB, the name at 128 bytes (cut on a character
   boundary). A DOCTYPE or any other `<!` declaration is refused, so no
   entity is ever declared, let alone expanded; only the five predefined
   entities and character references are decoded, any other entity
   refuses the name; nothing external is referenced. The decoded name must be
   strict UTF-8 without control characters, and not empty. No `Info`, two
   `Info` chunks, two `Name` attributes or any refusal leaves the file name
   as the name: a bad `Info` never drops a preset.

   The musical category (`MusicalCategory`) is **not** carried: a category
   per preset is a new member of the probe record, so a record version 4,
   the catalog cache with it, and the engine and UI that would show it. That
   is rejected for this item and listed as a follow-up.

2. **The engine-side wiring is deferred.** One `FactoryPresetReader` owned
   by the control server, and `forget_blocked()` called on `plugin.rescan`,
   need `engine/control/src/engine_control_server.cpp` and
   `engine/control/CMakeLists.txt`, which `time.elastic` and
   `ui.mixer.sidechain` hold. It is a follow-up feature once they release
   them. The other wiring line, keeping program-change parameters out of a
   preset's `params`, needs nothing: the host already skips them
   (`plugins/host/src/main.cpp`, `--read-preset`).

3. **Loading a preset is not in this item.** `ModelAction LoadPreset`
   (ADR-0029 slice 5) needs a protocol bump and a session format change; it
   stays a feature of its own.

## Rationale

- The name is the one field an operator reads in a list. It fits the
  record's existing `name` with no format change, so it ships now; anything
  more waits for a record version that can carry it.
- A scan for one attribute is small enough to read whole and to fuzz;
  a general XML parser, even one with expansion switched off, is a much
  larger surface for one string. The SDK writes the chunk flat, one
  `<Attr>` per line, so nothing the SDK writes is lost by refusing DTDs.
- Falling back to the file name, never refusing the preset, keeps the
  listing's behaviour for every file that listed before.

## Consequences

- `plugins_host_vstpreset_fuzz` covers the `Info` reader (with a name,
  without, truncated, over the caps, an entity bomb, invalid UTF-8, two
  `Info` chunks, and 100 000 mutated chunks); `plugins_host_read_preset`
  checks a probe lists a file by its `Info` name and one with a bomb by its
  file name.
- A listing reads up to 64 KiB more per matching file. At the record's cap
  of 4096 presets that is bounded and only paid at scan.
- `--read-preset` still answers a `.vstpreset` with its file name; the
  engine names the preset from the catalog's listing. Making the read answer
  use the `Info` name too is a `main.cpp` change for the follow-up.

## Alternatives rejected

- **A general XML parser (expat, libxml2) with entities off.** A new
  dependency in the host, and a surface far larger than the one attribute
  read; the bounded scan is enough for what the SDK writes.
- **Carry the category now as a free-form suffix of the name.** It would
  smuggle a second field through `name` and break the moment a record
  version 4 adds it properly.
- **Refuse a preset whose `Info` is malformed.** The state chunks are what
  the plugin loads; a broken label is no reason to hide a working preset.
- **Read the whole file to find `Info`.** A listing would read up to 17 MB
  per file; the header and the chunk list already say where `Info` is.

## Amendment 2026-11-13 - the server owns the reader

`EngineControlServer` owns one `FactoryPresetReader` when a plug-in host is
configured, built from the scan worker's host path, bundles and probe timeout
(`preset_reader()`). `plugin.rescan` calls `forget_blocked()` before it queues
the scan, so a preset that crashed its reader is tried again after a rescan
and not before. Preset load (ADR-0029 slice 5) is the reader's first caller;
until then only the test reads through it. No protocol or format change.
Test: `test_plugin_rescan_presets`.
