# Context: harness and CI

Local CI context: `harness/docs` (nothing heavy). Agent: harness-engineer.

## Where
- `tools/orchestrator.py`, `tools/harness/*.py` (scheduler, train, localci,
  launcher, store, plans), `tools/harness/tests`, `tools/local-ci.sh`,
  `tools/build-slot.sh`, `tools/Dockerfile.*`, `tools/build-*.sh`,
  `tools/dashboard.*`, `.github/`.
- Routing data for the orchestrator: `.claude/routing.toml`.

## Bound by
- ADR-0047: harness orchestrator (Python daemon, SQLite state machine,
  headless Sonnet roles).
- `.claude/PROCESS.md`; rules `gates-and-checks`, `build-lock-and-machine`,
  `branches-and-prs`.

## Build and test
- `tools/local-ci.sh --dry-run` prints contexts, steps and ctest filter.
- `--full` is main's run; `--os` the image; `--base REF` compares to REF.
- Lock: `tools/build-slot.sh --list`; lock dir `%LOCALAPPDATA%\sonora\build-lock`.
- Each step logs to `.local-ci/<step>.log`; the summary is one line per step.

## Traps
- Never touch a live daemon's checkout, database or lock.
- The path-to-context table in `tools/local-ci.sh` is the single source.
- A red product test is not a harness bug: route it (PROCESS.md).
