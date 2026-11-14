# ADR-0185: Surfaces contract verification (2026-11-13)

Status: accepted

## Context
ADR-0040 and #358 put `surface_link.h` and `kSurfaceContractVersion` on main; ADR-0120 recorded the closure. The item still read todo in progress.

## Decision
No new design. The contract stays as in ADR-0040. This ADR records the re-verification: `git log origin/main -S kSurfaceContractVersion` must show #358, and the item is closed by the coordinator's chore PR.

## Rationale
A reserved ADR number is used for the verification record rather than left a gap.

## Consequences
No code, protocol or format change.

## Alternatives rejected
Reopening the contract: nothing is missing. Leaving the item todo: it stalls the track.

## Evidence

- `git log origin/main --oneline -S kSurfaceContractVersion` prints:
  `2c11404d drivers.surfaces.contract: closed by reference to #358 (ADR-0040 status, ADR-0120 closure record) (#605)`
  and `1b955434 Control surface boundary: events in, feedback out over rings (ADR-0040) (#358)`.
  The contract landed in #358; #605 only edited ADR-0040 and added ADR-0120.
- `planning/progress.json` on main still reads `drivers.surfaces.contract` as `todo`;
  the `[x]` line and the progress change go in the PR body, not in this file.
- Draft ADR-0185 was carried over from commit bc3f1c80; its planning-file edits were not carried.
