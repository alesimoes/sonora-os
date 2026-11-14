# ADR-0089 - `system.install.adr` is closed by ADR-0032

- Status: Accepted
- Date: 2026-11-08
- Item: `system.install.adr`
- Relates to: ADR-0032 (installer), ADR-0002 (Debian base)

## Context

The backlog item "Installer ADR: Debian's, Calamares or our own" was still
open when ADR-0032 had already answered it (Accepted 2026-11-04). The
planner reserved a number for a new ADR before it found the earlier one.

## Decision

No new installer decision is taken. ADR-0032 stands: our own thin
`sonora-install` over Debian's tools. This record exists only so the item
closes by reference and the reserved number is not left dangling.

## Rationale

Re-deciding would risk contradicting ADR-0032 quietly; a pointer cannot.

## Consequences

`system.install.installer` builds against ADR-0032. ADR-0032's Unverified
list (hardware boot, initramfs on unseen controllers) is unchanged and still
needs the researcher's measurements.

## Alternatives rejected

- A second installer ADR restating ADR-0032: duplicates it and can drift.
- Leaving the item open: it would be dispatched again.
