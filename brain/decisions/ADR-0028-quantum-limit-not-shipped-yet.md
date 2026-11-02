# ADR-0028 — `default.clock.quantum-limit` is not shipped until a real device runs under it

- Status: Accepted (recommended default, taken by the coordinator with no
  owner available, 2026-11-02)
- Date: 2026-11-02
- Amends: ADR-0003, addendum of 2026-10-13

## Summary

The PipeWire drop-in (`audio-stack/pipewire-sonora.conf`) keeps PipeWire's
stock `default.clock.quantum-limit`. Setting it to 128 would close the one
measured quantum gap left, but it also sizes server-side buffers that no
test here has run against. The gap is left open and reported, and the
engine already ends a take the gap could reach. The decision is reopened by
the first run on a real ALSA device, below.

## Context

`tools/test-quantum-pin.sh` measures 17 routes by which a client could move
the graph off 128 frames / 48 kHz; all hold (`brain/research/quantum-pin.md`,
on PipeWire 1.4.2, and again on 1.6.9 per PR #238). Two gaps remain, both
needing a deliberate two-step edit of the settings metadata that no JACK call
makes:

1. `clock.max-quantum 256`, then `clock.force-quantum 256`: the graph runs at
   256.
2. `clock.allowed-rates [ 48000 96000 ]`, then `clock.force-rate 96000`: the
   graph runs at 96 kHz. Nothing in PipeWire's configuration closes this one.

`default.clock.quantum-limit = 128` closes the first: the limit bounds a
forced quantum where `max-quantum` does not. But the limit is also what the
server sizes buffers by: the ALSA node's and the adapter's resampler buffers
for a device that cannot run at 48 kHz, which then converts more than 128
device frames per cycle. The container's dummy driver and null sinks have no
period-size or rate constraints, so a pass there says nothing about this.

## Decision

Do not ship `default.clock.quantum-limit` yet. Keep the first gap reported
as `QUANTUM gap` by `tools/test-quantum-pin.sh`, not failed.

## Rationale

- The cost of the gap is bounded. Reaching it needs an operator, or a
  program, editing the settings metadata twice on purpose. And the engine
  does not record through it silently: since ADR-0007 M2 a block-size or
  rate change during a take is fatal to the take (`PeriodGuard`). A stopped
  take, with the reason named, on a route no client takes by accident.
- The cost of a wrong limit is not bounded the same way. If the limit
  starves the ALSA node or the resampler on some interface, every session on
  that machine xruns or fails to start, and nothing in the repository can
  show it before a device does. "Boot on hardware we have not seen" weighs
  more than closing an edit that needs two deliberate steps.
- PipeWire's own default limit is large (8192), so leaving it changes
  nothing that has been measured.

## Consequences

- `tools/test-quantum-pin.sh` keeps reporting gap 1; it is not a failure.
- Reopen when either is available, and record the result in
  `brain/research/quantum-pin.md` with the hardware named:
  - the UMC1820 as graph driver (the Phase 1 line repeating
    `test-quantum-pin.sh` on it), run once with and once without
    `quantum-limit = 128`, buffer size and xrun count each time;
  - a device that cannot run at 48 kHz (44.1 kHz-only or 96 kHz-only), so
    the resampler path is exercised under the limit.
  If both run clean with the limit, ship it and move gap 1 to the closed
  routes in `test-quantum-pin.sh`.
- Gap 2 stays open whatever is decided here; the engine's rate check is
  what stands behind it.
