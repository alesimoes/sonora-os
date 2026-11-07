# Context: harness and CI

The micro build builds nothing for these paths. Agent: harness-engineer.

## Where
- `tools/orchestrator.py`, `tools/harness/*.py` (scheduler, train, nightly,
  launcher, store, plans), `tools/harness/tests`, `tools/micro-build.sh`,
  `tools/build-slot.sh`, `tools/Dockerfile.*`, `tools/build-*.sh`,
  `tools/dashboard.*`, `.github/`.
- Routing data for the orchestrator: `.claude/routing.toml`.

## Bound by
- ADR-0047: harness orchestrator (Python daemon, SQLite state machine,
  headless roles).
- `.claude/PROCESS.md`; rules `gates-and-checks`, `build-lock-and-machine`,
  `branches-and-prs`.

## Build and test
- `tools/micro-build.sh --dry-run` prints the changed paths and the image;
  without it, the plan (targets, tests, why) lands in `.micro-build/plan.json`.
- `--base REF` compares to REF; `--rtsan`/`--no-rtsan` force the RT stage.
- Lock: `tools/build-slot.sh --list`; lock dir `%LOCALAPPDATA%\sonora\build-lock`.
- The nightly (`.github/workflows/nightly.yml`) is main's full run; `gh run
  list --workflow nightly.yml` shows it.

## Traps
- Never touch a live daemon's checkout, database or lock.
- The target graph (CMake file API) is the single source of what a change
  reaches; `ci.yml`'s `changes` only picks the image and the `rt` flag.
- A red product test is not a harness bug: route it (PROCESS.md).
