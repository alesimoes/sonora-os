# ADR-0147 - `plugins.host.adr` is closed by ADR-0007 section 4.3 and ADR-0012

- Status: Accepted
- Date: 2026-11-09
- Item: `plugins.host.adr`
- Relates to: ADR-0007 (section 4.3, plugins out of process), ADR-0012 (plugin hosting)

## Context

The backlog item "Plugin boundary ADR, after the IPC measurements" waited on
the ADR-0007 IPC figures. Those figures were measured in a container
(`brain/research/adr-0007-measurements.md` sections 3 and 4) and the boundary
was decided afterwards: one host process per insert chain, a futex wake per
callback (gated off until measured), a missed block plays silence, `HostControl`
over a SOCK_SEQPACKET socketpair, the scan worker and catalog (ADR-0012,
Accepted in part 2026-10-13, with amendments through 2026-11-03).

## Decision

No new boundary decision is taken. ADR-0007 4.3 and ADR-0012 stand. This
record closes the item by reference and uses the reserved number.

## Rationale

A second boundary ADR would restate ADR-0012 and could drift from it.

## Consequences

The bare-metal IPC measurement (RT kernel, 18 channels at quantum 128, xrun
attribution) is still open under "Blocked on hardware" in the BACKLOG and is
the researcher's; until it passes, ADR-0012 section 1 stays provisional and the
wake stays gated off. This ADR does not change that.

## Alternatives rejected

- A new ADR restating ADR-0012: duplicates and drifts.
- Keeping the item open: it would be dispatched again.
