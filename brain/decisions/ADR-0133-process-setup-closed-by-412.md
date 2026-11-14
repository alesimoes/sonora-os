# ADR-0133 - `architecture.portability.process_setup` is closed by reference to #412

- Status: Accepted
- Date: 2026-11-09
- Item: `architecture.portability.process_setup` (ADR-0008 P3)
- Relates to: ADR-0008 (portability, section 5, step P3), ADR-0121 (same closure for P1/P2)

## Context

The BACKLOG item "P3: an `engine/app/include/sonora/process_setup.h` seam,
covering `lock_memory()`, `install_quit_handlers()` and
`default_session_root()`, implementations under `engine/app/src/posix/`" is
`todo` in `progress.json`. Its work landed in PR #412 (commit `cc2d26d7`,
"Portability layer ... process_setup seam ... (ADR-0008 P1-P3)").

#412 placed the seam in `common/platform`, not `engine/app`, so the item's
paths are stale but its content is delivered:

- `common/platform/include/sonora/platform/process_setup.h` declares
  `lock_memory(std::string& why)`, `install_quit_handlers()` and
  `default_session_root(const char* home)`.
- Implementations: `common/platform/src/posix/process_setup.cpp` and
  `common/platform/src/unsupported/process_setup.cpp` (the neutral build).
- `engine/app/src/main.cpp` calls all three through `sonora::platform::`; no
  `mlockall`, `sigaction` or `signal` call remains in `engine/app/src`.
- Tests: `common/platform/tests/test_process_setup.cpp`,
  `test_memory_lock.cpp` (refusal names the limit) and `test_quit_signals.cpp`.

Checked on `origin/main` at `e5344b6b` with `git ls-files` and
`grep -rnE 'mlockall|sigaction' engine/app/src`.

## Decision

1. The item is closed by reference to #412. The closing PR ticks the line and
   names #412 and this ADR. No code is written.
2. The location `common/platform` stands. ADR-0008 wants platform code at the
   edges behind a seam; a shared module lets `engine/app` and the UI use the
   seam without depending on each other. Moving it to `engine/app` would
   contradict the dependency DAG (ADR-0007).
3. A POSIX call found later in `engine/app/src` is a new item.

## Rationale

- The work is merged and tested; redoing it duplicates the seam.
- The BACKLOG path was written before #412 chose the better home.

## Consequences

- The BACKLOG line is ticked with a note that the header lives in
  `common/platform`.
- Unverified: the tests were not run by the architect; they run on the PR gate
  only if their files change, and the nightly runs them.

## Alternatives rejected

- Re-implement under `engine/app/`: duplicates the seam and breaks the DAG.
- Leave the box open: the item keeps reappearing in the queue and costs a plan
  per wake.
