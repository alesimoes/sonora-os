# ADR-0221: Amp and cabinet captures, close-out of the tooling slice

Status: Accepted (eng-12, 2026-11-14; recommended default, nothing here needs the owner's decision).

Builds on: ADR-0011 §4.6 (captures and IRs we ship are our own or licensed and recorded on a card) and §5 (names), ADR-0069 (capture cards and the capture manifest), ADR-0061 (micro builds, the PR gate).

## Summary

The tooling and policy for amp and cabinet captures are on `main`: the capture
rig procedure and held-out ESR method (`brain/research/ampcab-capture-rig.md`,
`f137ccf4`), the capture card contract and its checker (ADR-0069, `3c7bfeea`),
the card and IR templates, the name deny list and the tests (`native_ampcab_*`),
merged through #534. What remains is captures from the owner's hardware. Those
are parked: no capture or IR is produced, shipped or pinned until the owner
names the unit. This ADR records that the tooling part is closed and that the
capture part waits on hardware, so the backlog line is not read as done.

## Context

`ampcab.captures` was one backlog item covering two different kinds of work:
code and policy (done, on main) and recording (needs the owner's amps and
cabinets, which no agent can supply). Leaving the item open kept an agent
looking for a capture to produce. Closing it outright would hide that the
shipped amp and cabinet family still has no real capture.

## Decision

1. **The tooling and policy slice is closed.** Its deliverables are on `main`
   and are the reference for any later capture: the rig procedure, the
   held-out ESR and IR methods, the card contract (`parse_card`,
   `validate_card`, `check_capture_file`), the checker and the tests.
2. **The captures themselves stay parked** until the owner provides the
   hardware (ADR-0011 §4.6). Each capture arrives as its own change: a card in
   `brain/models/<id>.md` first, then the file's size and SHA-256 pinned in
   `plugins/native/ampcab/captures/manifest.json`. Capture files are not in git.
3. **No capture is produced, copied or fitted** to any commercial unit's
   output (ADR-0004, ADR-0011 §5). A unit is recorded only from hardware the
   owner owns or rents with recording allowed.
4. **Docs only.** This close-out changes `planning/` and `brain/` and no
   source. The BACKLOG line is ticked by the coordinator's chore PR, with its
   text split so that no ticked line names the open hardware part.

## Rationale

The checks that stop a bad capture from shipping exist and pass on main, so
nothing about the tooling is waiting on work. The one open part needs a
physical unit, which is the owner's to supply. Writing that down keeps the
backlog honest without inventing a capture.

## Consequences

- The `ampcab.captures` backlog line becomes "tooling done; captures parked to
  owner hardware". Nothing in `plugins/native/ampcab/captures/` changes now.
- A capture added later is one PR per unit: card, manifest entry, golden and
  the harness report, and it must meet `quality.json` before it is claimed.
- `ampcab.budget`'s open item (re-run on a quiet machine with the 200 ms IR,
  Unverified 9 in `brain/research/native-ampcab-n3.md`) is a separate item and
  is not closed by this ADR.

## Alternatives rejected

- **Keep the item open as one piece.** It asks an agent for hardware it cannot
  have, and it hides that the tooling is finished.
- **Close it outright.** It would claim the amp and cabinet family has real
  captures. It does not.
- **Ship a stand-in capture from a published schematic or a public IR.** A
  published circuit is not a recording of a unit, and a public IR is a
  third-party asset (ADR-0011 §4.6). Refused.
