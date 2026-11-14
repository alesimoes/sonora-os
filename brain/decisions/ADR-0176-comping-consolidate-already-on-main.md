# ADR-0176: Comp-to-playlist consolidate is already on main

Status: accepted (2026-11-13)

## Context

The backlog item `ui.comping.consolidate` asks for comp-to-playlist and
consolidate: render a comp range to a new BWF and place it as one compound
clip, per ADR-0031 §6. The item was still open on the backlog, but the code
and tests landed earlier (#344 for the engine half, and the comping UI and
model tests since).

## Decision

The item is done on `main`. No source change is made. The verification run
re-executes the existing tests through `tools/micro-build.sh`, and the
coordinator ticks the backlog line on that evidence.

## Evidence

- Files on `origin/main`: `engine/control/src/consolidate.cpp`,
  `engine/control/src/consolidate_job.cpp`,
  `contracts/api/include/sonora/api/consolidate_argument.h`,
  `ui/qt/src/scenes/edit_comping_consolidate.cpp`.
- Micro-build over the three test sources: `consolidate`, `model_comping` and
  `ui_comping` all pass (100%, 0 failed of 3).

## Consequences

- Only the backlog line changes, by the coordinator.
- Any later consolidate work is a new item, not a reopening of this one.
