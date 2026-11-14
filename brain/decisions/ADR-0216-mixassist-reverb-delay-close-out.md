# ADR-0216 - Close out mixassist.reverb_delay: delivered by ADR-0128, two follow-ups open

Status: accepted (eng-69, 2026-11-14)

## Context
Backlog item `mixassist.reverb_delay` asks for tempo-synced reverb and delay
from a reference. ADR-0128 records that the planner already ships on main
(`ReverbDelayPlanner` and `ReverbDelayReference` in
`engine/control/include/sonora/mix_planner.h`, merged in #437) and that a
verification-only slice runs `test_mix_planner`. The verification record is
`brain/research/mixassist-reverb-delay-verification.md`.

This closeout re-runs the planner test on the current branch so the item can
close on evidence rather than on the checkbox alone. It changes no code.

## Decision
Close `mixassist.reverb_delay` as delivered by ADR-0128 and #437. Keep the two
gaps ADR-0128 named as explicit follow-ups, not as part of this item:

1. **Aux-bus and master inserts.** The planner's sends are proposals only.
   Aux buses hold no inserts yet (ADR-0045 §5), so the processor settings in
   the plan are not applied to audio. Applying them waits on Aux and Master
   inserts and is a separate backlog line.
2. **RT60 and pre-delay measurement.** The planner takes reference numbers and
   does not measure RT60 or pre-delay from audio. An analysis pass for that
   belongs to `sonora-analyze` and is a separate backlog line.

## Verification run (this slice)
- Command: `tools/micro-build.sh --no-rtsan --files-from <file naming
  engine/control/tests/test_mix_planner.cpp>`, which builds
  `sonora_control` and `test_mix_planner` and runs `ctest -R '^(mix_planner)$'`.
- Output: `1/1 Test #712: mix_planner ...... Passed 0.90 sec`,
  `100% tests passed, 0 tests failed out of 1`, `micro-build: passed`.
- Host: Windows 11 Enterprise, build in the `sonora-build:trixie` container,
  CCACHE hit rate 160/161 at the time of the run.
- The wrapper's shell exit was 124 (the 9-minute cap the agent set) after
  the line `micro-build: passed` had printed; the test result is taken from
  that output, not from the exit code.

## Consequences
- The checkbox for `mixassist.reverb_delay` may be ticked by the coordinator's
  chore PR. The two follow-ups above go into BACKLOG as their own lines.
- No code changes. No processor settings reach audio from this item.
- The planner's snapping tables still await a listening check on real
  material (ADR-0128, researcher).

## Alternatives rejected
- Keep the item open until the inserts land: that ties a planner that already
  ships to a separate processor feature.
- Fold RT60 measurement into this item: it needs audio analysis and a
  reference set, which is new work.
