# ADR-0199: plugins.offline close-out (Process Selection With is complete)

Status: accepted (2026-11-14)

## Summary

*Navigation only; the sections below are the decision.*

- `plugins.offline` is complete end to end: Track > Process Selection With
  renders one chosen insert of a track in real time, on the live chain, as
  ADR-0051 §2 describes. Contract, engine, UI and control-server wiring are
  all on `main`.
- Faster-than-real-time rendering through hosted plug-ins is not built and
  stays out of scope. Its one open question (a worker thread above 1x with
  the hosts out of process) is a measurement for the researcher, recorded in
  ADR-0087 Unverified and repeated below.

## Context

ADR-0087 split `plugins.offline` into the parts that landed separately:

- #625 built the engine half (`RenderOptions::only_insert`, the render-scoped
  silencing of the other inserts in `PluginHosting`, the plan in
  `RenderInPlace`), but left `engine_control_server.cpp` on the command-blind
  parse because another task held that file.
- #707 wired the control server (the `insert=<id>` word for
  `track.process_selection` only, `track.render.insert` announced in Hello)
  and amended ADR-0087 to say so.
- The UI entry, the capability constant and the render tests were already on
  `main` with those changes.

What remained was the record: no decision says the feature is complete, and
the progress item still reads as open. This ADR is that record. It changes no
code.

## Decision

1. `plugins.offline` is closed as complete. The feature is Process Selection
   With: one insert of one track, rendered in real time, through the
   `track.process_selection` command with `insert=<id>`.
2. The evidence on `main` that the feature is whole:
   - contract: `kCapabilityRenderInsert = "track.render.insert"` in
     `contracts/api/include/sonora/api/render.h`, with its wire tests in
     `contracts/api/tests/test_render_wire.cpp`;
   - engine: `RenderOptions::only_insert` and `RenderPlacement::only_insert`
     in `engine/control/include/sonora/render_in_place.h`, and
     `render_insert_valid()` in `engine/control/src/render_in_place.cpp`,
     the silencing override `engage_solo()` in
     `engine/control/src/render_solo.cpp`;
   - control server: `engine/control/src/engine_control_server.cpp` reads
     `insert=` through the command-aware parse, only for
     `track.process_selection`;
   - UI: `ShellModel::renderInsertAvailable()` in `ui/qt/src/shell_model.cpp`
     gates the Track menu entry `trackProcessWith` in `ui/qt/qml/Main.qml`.
3. Faster-than-real-time rendering through hosted plug-ins is out of scope
   for `plugins.offline`. It is not a deferred part of this item; it needs a
   separate decision, after the measurement below.

## Rationale

- ADR-0004 keeps plug-ins out of process, and ADR-0012 makes any deadline
  miss fail a render. A faster-than-real-time host would drive the same hosts
  from a worker thread at more than 1x, a case nothing has measured. Building
  it now would ship an unmeasured path.
- Real-time Process Selection already covers the operator's need (print one
  insert of a track) within the existing latency and isolation rules. The
  close-out therefore does not need a new path to be useful.
- Recording the close-out as its own ADR lets the progress item close without
  reopening ADR-0087, whose amendment already says what was wired.

## Consequences

- The `plugins.offline` progress item can be marked complete. Its remaining
  open question is a researcher measurement, not a build task.
- Faster-than-real-time Process stays unbuilt. Reopening it requires a yes
  from the measurement in Unverified below; a no closes it for good.
- No protocol, format, contract or code change. The capability and the
  `insert=` word are the ones ADR-0087 §2 already fixed.

## Alternatives rejected

- **Keep `plugins.offline` open until faster-than-real-time Process exists.**
  Rejected: that work is not part of the feature the operator asked for, and
  it has no owner decision or measurement yet. Keeping the item open would
  hide a complete feature behind an unscheduled one.
- **Build the offline render through a second host driver now.** Rejected:
  it puts a worker thread at more than 1x against hosts that are out of
  process, without the measurement ADR-0087 asks for.
- **Reopen ADR-0087 instead of writing this record.** Rejected: ADR-0087
  already carries its amendment and Unverified section; a new ADR states the
  closure once, with its evidence, and leaves ADR-0087 as the decision it is.

## Unverified

- **Offline host measurement (for the researcher; ADR-0087 Unverified):** can
  a host process run a plug-in chain driven from a worker thread at more than
  1x real time without breaking ADR-0004 (isolation, hosts out of process) and
  ADR-0012 (the lock-free, deadline-bound handoff; any miss fails a render)?
  Measure on the target machine with real LV2 and VST3 hosts: throughput,
  misses, and whether the live callback's hosts are disturbed. A yes reopens
  faster-than-real-time Process; a no closes it. Not a blocker for this
  close-out.
- **Override on real hosts (ADR-0087 Unverified, researcher):** that a
  silenced insert confirms within the pre-roll on real out-of-process LV2 and
  VST3 hosts, and that the print of one insert nulls against the live output
  with the others bypassed by hand. CI covers only eg-amp under
  `sonora-plugin-host`, where that host is built.
- **`plugin_hosting` (the real-host suite) was not run by this close-out.**
  `test_plugin_hosting` is built only where `sonora-plugin-host` is (CI's
  plugins job, `tools/build-plugins.sh`); the micro-build on this record ran
  `plugin_hosting_state_wait` only. The `control_process_with_host` check
  (eg-amp at 0 dB beside -90 dB) is therefore not re-run here.
