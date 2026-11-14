# ui.comping.consolidate — plan (already done on main)

Status: verified done on `main` (2026-11-13). No source change is needed.

## What the item asked for

Comp-to-playlist and consolidate: render a comp range to a new BWF and place it
as one compound clip (ADR-0031 §6), with the UI action that drives it.

## Where it lives on main

- Engine: `engine/control/include/sonora/consolidate.h`,
  `engine/control/include/sonora/consolidate_job.h`,
  `engine/control/src/consolidate.cpp`, `engine/control/src/consolidate_job.cpp`
  (landed in #344, "Consolidate, engine half").
- Contract: `contracts/api/include/sonora/api/consolidate_argument.h`.
- UI: `ui/qt/src/consolidate_send.h`, `ui/qt/src/scenes/edit_comping_consolidate.cpp`.
- Tests: `engine/control/tests/test_consolidate.cpp` (ctest `consolidate`),
  `common/model/tests/test_comping.cpp` (ctest `model_comping`),
  `ui/qt/tests/test_comping.cpp` (ctest `ui_comping`).

## Verification (this run)

`tools/micro-build.sh --files-from` over the three test sources, image
`sonora-ui`, job `20261113-212752-184641`: build exit 0, then
`ctest -R '^(consolidate|model_comping|ui_comping)$'` →
100% passed, 0 failed out of 3.

## Sections

- Contracts: none new (already on main).
- Engine / UI / tests / docs: already on main; N/A for this item.
- Open questions: none.
