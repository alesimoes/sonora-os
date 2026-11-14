# ADR-0127 - `interchange.sessions.convert` is closed by sonora-convert (ADR-0007 M10, #375)

- Status: Accepted
- Date: 2026-11-09
- Item: `interchange.sessions.convert`
- Relates to: ADR-0007 (section 10, milestone M10), ADR-0121 (same closure pattern)

## Context

The BACKLOG item asks for the session conversion tools: SMF import, SMF
export and session rewrite. The work landed on `main` in PR #375 (commit
`14277e3a`, "sonora-convert: SMF import/export and session rewrite (ADR-0007
M10)"). The `sonora-convert` target is in `interchange/` (`src/smf_import.cpp`,
`src/smf_export.cpp`, `src/session_io.cpp`, `src/convert.cpp`, the app in
`app/main.cpp`), and its tests are in `interchange/tests/test_interchange.cpp`.

## Decision

The item is closed by reference to #375. No code changes are made in the
closing PR. The closure is a documentation confirmation only.

## Checks

Run on this branch, based on `origin/main`:

- `tools/micro-build.sh --files-from <git ls-files interchange>`: the
  `sonora_interchange` and `test_interchange` targets build; the `interchange`
  ctest passes (`100% tests passed, 0 tests failed out of 1`, 51.86 s), and
  the run ends with `micro-build: all passed`.
- `git log origin/main --oneline -S sonora-convert` finds `14277e3a` (#375).

## Consequences

- The BACKLOG line for `interchange.sessions.convert` is ticked by the
  coordinator, not by this PR.
- Any further conversion work (for example the AAF round trip) is a new
  BACKLOG item, not a reopening of this one.
