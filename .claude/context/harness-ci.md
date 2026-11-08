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

## The console (dashboard at :8765, owner 2026-11-08)
- One screen, spec in `docs/design/dashboard-console/SPEC.md`. Dashboard side:
  `tools/dashboard.py`, `dashboard_console*.py`, `dashboard-static/console-*`.
  It only reads and posts requests; the daemon alone writes `.harness/console/`.
- Daemon side: `harness/console.py` (requests, the answering run, replies),
  `console_verbs.py` (the whitelist: add a verb there, with a test, or it does
  not exist), `console_actions.py` (cards: confirm runs once), `console_store.py`.
- The `console` agent (`.claude/agents/console.md`) reads through
  `tools/console_read.py`, answers with `send(type=console_reply)`, never runs
  an action. A new message type or verb needs a test in `test_console_*.py`.
