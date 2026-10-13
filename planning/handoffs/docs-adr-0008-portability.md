# docs/adr-0008-portability

- **Goal:** record the owner's 2026-10-12 portability decisions as ADR-0008,
  plus the backlog and catalogue items they create.
- **State:** done. The branch is pushed, with no PR yet (the coordinator's call).
- **Commits:** see `git log origin/main..docs/adr-0008-portability`.

## Verify
    tools/progress.sh --check   # expected: progress: catalogue valid, PROGRESS.md current

## Done
- ADR written — `brain/decisions/ADR-0008-portability.md`. It covers the
  rule (§1), the audit with 5 violations and 2 build issues (§2), the
  platform selection and seams (§3), enforcement and the CI choice (§4),
  migration P1–P10 (§5), and the open Windows questions (§6).
- Index line added — `brain/INDEX.md`.
- Pointer note added to ADR-0007 — section "Update, 2026-10-12: portability".
- P1–P10 plus the ASIO licence research item are in `planning/BACKLOG.md`,
  section "ADR-0008 portability".
- 11 items are in `planning/progress.json`, under `architecture.portability`.
  `PROGRESS.md` has been regenerated.

## Open
- Before the merge gate, delete this file and update `main-session.md`.
- Audit line numbers for V5 (`feature/qt-shell` 65f54d1) and for
  `memory_lock` (`contract/backend` 0e74180) refer to those unmerged branches.

## Do not redo
- Mingw and macOS were evaluated and rejected for the CI job (ADR §4).

## Pointers
- `brain/decisions/ADR-0007-modules-contracts-and-squads.md` §1, §9 (M2–M7)
