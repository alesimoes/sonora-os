# ADR-0001 — Write the engine from scratch

- Status: Accepted
- Date: 2026-10-11
- Decided by: project owner

## Context

Two ways to get a multitrack recorder: fork Ardour (GPL, mature engine, already
reads `.ptx` through libptformat) or write the engine ourselves.

Forking lands a working DAW in weeks. Writing our own means rebuilding things
Ardour took twenty years to get right: sample-accurate transport, plugin
hosting, audio undo, comping, varispeed, ADAT and word-clock sync.

## Decision

Write the engine from scratch.

## Consequences

Accepted cost: the honest timeline to a tool an engineer would trust with a
paid session is measured in years, not months. We own that.

What we buy: an architecture shaped around booting as an appliance rather than
running as a desktop application, and no inherited constraints in the data
model.

What this obliges us to do, from the first commit:

- Lock-free real-time path, no allocation in the callback. Retrofitting this is
  a rewrite, so it is a rule from day one (see `CLAUDE.md`).
- Disk I/O on its own thread, with overruns counted rather than hidden.
- Tests on the data path before features on top of it.

## Alternatives rejected

**Fork Ardour.** Rejected by the owner. Worth recording that it remains the
fastest path to a usable product, and that the Ardour engine is a reference we
may read for approach — it is GPL, and so are we.
