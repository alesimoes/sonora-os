# docs/phase2-session-timeline

- **Goal:** Phase 2 session format and timeline model specs before any code
  (BACKLOG Phase 2, "Session file format spec"; ADR-0007 M8).
- **State:** in review. Docs only; no code, nothing to build.
- **Commits:** see `git log origin/main..HEAD`.

## Verify
    python tools/progress.py --check   # expected: progress: catalogue valid, PROGRESS.md current

## Done
- Session file spec: `brain/specs/session-format.md`.
- Timeline model, undo, plan spec: `brain/specs/timeline-model.md`.
- ADR-0009 (Proposed): `brain/decisions/ADR-0009-timeline-snapshots-and-plan-swap.md`.
- Owner decision (fixed 2-channel monitor bus, 2026-10-12) recorded in
  `planning/ROADMAP.md` Phase 2 and the spec; removed from STATUS "Next
  decision needed".
- Catalogue: `engine.session.spec`, `engine.timeline.spec` doing on this
  branch; `engine.session.json` added; notes on mix_out, disk_read, punch.

## Open
- ADR-0009 needs review; it adds `common/json` to ADR-0007's graph (additive).
- References ADR-0008 (portability), which is on `docs/adr-0008-portability`,
  not yet on `main`.
- On merge: mark `engine.session.spec` and `engine.timeline.spec` done with
  the PR number as evidence; delete this file.

## Pointers
- ADR-0007 §2 (plan swap), M8; ADR-0008 §1, `common/platform`.
