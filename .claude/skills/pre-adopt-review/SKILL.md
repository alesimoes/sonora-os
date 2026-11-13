---
name: pre-adopt-review
description: What the coordinator checks in an agent's PR before the orchestrator adopts it. Use when an agent reports a finished PR.
---

# Pre-adopt review

Cheap checks on the diff and the report, not a code review (that is
`code-reviewer`). Any failure sends it back to the same specialist.

1. Right agent: paths match `.claude/routing.toml`; an approved plan exists in
   `planning/plans/<id>.md` and the PR follows its slices.
2. The diff disables nothing: grep it for removed or added `DISABLED`,
   `GTEST_SKIP`, `LABELS slow|timing` on an existing test, ctest `-E`/`-LE`,
   sanitizer flags, `|| true`, bigger timeouts, gate or workflow edits.
3. Tests claimed actually ran: the PR body names a command and what it
   printed; the test files exist in the diff; unverified items are listed.
4. Size: `git diff --stat`; no file grown past about 500 lines (or split in
   the same PR); refactor and feature not mixed.
5. Fresh branch from `origin/main` (`git merge-base` is a recent main
   commit), no stale commits, no merge commits.
6. `tools/pre-pr.sh` was run (whitespace, exec bits, ADR numbers, no BACKLOG
   edit, reserved numbers).
7. UI change: no generated screenshots committed (they are ignored).
8. Docs and ADR travel with the code; contract changes have fake + test.

Result: adopt, or send back with the failed item numbers. Never fix it
yourself and never merge.
