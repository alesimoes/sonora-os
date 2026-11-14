# interchange.sessions.convert — plan (already done on main)

Status: verified done on `main` (2026-11-14). No source change is needed.

## What the item asked for

The session conversion tools: SMF import, SMF export and session rewrite, as
the offline `sonora-convert` process (ADR-0007 section 10, milestone M10).

## Where it lives on main

- Tool: `interchange/app/main.cpp` (`add_executable(sonora-convert ...)` in
  `interchange/CMakeLists.txt`).
- Library `sonora_interchange`: `interchange/src/convert.cpp`,
  `interchange/src/smf_import.cpp`, `interchange/src/smf_export.cpp`,
  `interchange/src/session_io.cpp`.
- Tests: `interchange/tests/test_interchange.cpp` (ctest `interchange`).
- Landed in #375 (`14277e3a`, "sonora-convert: SMF import/export and session
  rewrite (ADR-0007 M10)"). Extended since by #688 (AAF export seam), #713
  (`.omf` import) and #717 (`.ptx` import).
- Closure record: ADR-0127 (merged as #611).

## Verification (this run)

On `97e44c96` (`origin/main` at the time; the one commit since, #737, adds an
ADR only):

- `git log origin/main --oneline -S sonora-convert` lists `14277e3a` (#375),
  and `14277e3a` is an ancestor of `origin/main`.
- `tools/micro-build.sh --no-rtsan --files-from F`, with F naming the four
  library sources above, image `sonora-build`: 16 targets built (among them
  `sonora_interchange` and `test_interchange`), then
  `ctest -R '^(aaf_contract|...|interchange|...|omf_reader)$'` →
  `100% tests passed, 0 tests failed out of 14`; `interchange` itself
  `Passed 14.33 sec`; the run ends `micro-build: all passed`.
- RTSan stage not run: `interchange/` is offline code, outside the real-time
  call graph.

## Decisions

- ADR: none new. ADR-0127 already records the closure; the number reserved
  for this task (0184) stays unused rather than carry a second ADR that
  only points at the first.
- The BACKLOG line and the progress entry are the coordinator's to set
  (evidence: #375, ADR-0127, this file).

## Sections

- Contracts: none new (already on main).
- Engine / UI / plugins / docs: N/A, already on main.
- Tests: already on main (`interchange`); re-run above.
- Open questions: none.
