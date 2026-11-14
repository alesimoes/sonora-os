# ADR-0227 — Loop browser closure

Status: accepted.

## Summary

- The loop browser feature (`logic.loop_browser`) is complete on `main`. It
  landed through PR #686 (merge commit `dd7406fe`), which carried the contract,
  engine and UI slices together with their tests and screenshots.
- The decisions are in ADR-0163 (loop catalog interfaces). This ADR adds no
  new decision: it closes the feature and records where its pieces live.
- No code, protocol or file-format change comes with this closure.
  `kProtocolVersion` is untouched.

## Decision

- `logic.loop_browser` is closed. Its plan is closed in
  `planning/plans/logic.loop_browser.md`.
- Anything further on loops (new filters, audition behaviour, inserter
  options) is a new backlog item with its own plan, not a reopening of this
  one.
- The closure is docs only. Its PR touches `brain/decisions/` and
  `planning/plans/` and nothing under `engine/`, `ui/`, `common/` or
  `contracts/`.
