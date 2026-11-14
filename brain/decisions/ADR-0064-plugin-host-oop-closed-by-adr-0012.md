# ADR-0064 - `plugins.host.oop` is closed by ADR-0007 section 4.3 and ADR-0012

- Status: Accepted
- Date: 2026-11-09
- Item: `plugins.host.oop`
- Relates to: ADR-0007 (section 4.3, plugins out of process), ADR-0012 (plugin hosting, checklist items 1 to 5), ADR-0147 (the same closure for `plugins.host.adr`)

## Context

The backlog item "out-of-process plugin host" was the engine half of the plugin
boundary: the host processes, the lock-free handoff to the audio thread, the
control-server wiring and the chain runner that calls them. ADR-0012 fixes the
design (one host process per insert chain, `HostControl` over a
SOCK_SEQPACKET socketpair, a missed block plays silence, the futex wake gated
off until measured). Its implementation checklist runs in order, and items 1 to
5 are now on `origin/main`:

- item 1, contract: `contracts/plugin/include/sonora/plugin/host_control.h`;
- item 2 and 3, model and plan nodes: `PluginChainRunner` is declared in
  `engine/control/include/sonora/plugin_hosting.h` and called from `engine/core`;
- item 4, `engine/plugin_client`: `host_supervisor`, `rt.cpp`, the
  `kHostWakeEnabled` gate (asserted false by `test_plugin_client.cpp`), and the
  host-process tests `test_plugin_client_host.cpp` and `test_rtsan_wake.cpp`;
- item 5, control-server wiring: `engine/control/src/plugin_hosting.cpp`
  includes `sonora/plugin/host_control.h`, and `plugins/host` (the host
  process, its IPC, editors and chain code) is merged and carries the
  timer-paced default.

So the out-of-process host is built and on main. What remains is a measurement,
not a boundary decision.

## Decision

No new boundary decision is taken. ADR-0007 4.3 and ADR-0012 stand, and the
`plugins.host.oop` item is closed by reference to them. This record uses the
reserved number. The engine host is not re-specified here, and no contract,
protocol or format changes.

## Rationale

The item was a build item whose design was already decided in ADR-0012. A
second record restating the boundary would drift from it, and a closed item
still open on the board is dispatched again. Closing it by reference keeps one
source of truth for the boundary and points the open measurement at the one
place that can settle it.

## Consequences

Still open, and not decided here:

- The bare-metal IPC measurement (RT kernel, 18 channels at quantum 128, xrun
  attribution) is under "Blocked on hardware" in the BACKLOG. It belongs to the
  researcher. Until it passes, ADR-0012 section 1 stays provisional and the
  futex wake stays gated off (`kHostWakeEnabled = false`). Hosts run
  timer-paced by default. This ADR changes none of that.
- Until the measurement passes, the latency figures in ADR-0012 and
  `brain/research/adr-0007-measurements.md` are container figures and are not
  to be quoted as the appliance's numbers.
- Any future work on the host (for example a new wake mode) is a new item that
  cites ADR-0012, not a reopening of this one.

## Alternatives rejected

- A new ADR restating ADR-0012 for the host: duplicates and drifts.
- Keeping the item open until the bare-metal measurement: the build is done and
  the measurement has its own owner and item; the open part is tracked there.
- Flipping `kHostWakeEnabled` to true to close the item: that is a decision the
  measurement must make, and it would be a side edit to an ADR-0012 provisional
  choice.
