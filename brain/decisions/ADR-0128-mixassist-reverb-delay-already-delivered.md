# ADR-0128 - Reverb and delay assistant: delivered in #437, verified here

Status: accepted (architect, 2026-11-09)

## Context
Backlog item `mixassist.reverb_delay` asks for tempo-synced reverb and delay
from a reference: decay, pre-delay, wet level and an excess check. The
ADR-0011 stage N5 assistant already ships `ReverbDelayPlanner` and
`ReverbDelayReference` (`engine/control/include/sonora/mix_planner.h`,
`src/mix_planner.cpp`) with tests in `test_mix_planner.cpp`, reachable as
`propose_mix_plan(MixPlanKind::Space | All)`. The checkbox is stale.

## Decision
Build nothing new. One docs-only slice builds and runs `test_mix_planner`
and records the result in `brain/research/mixassist-reverb-delay-verification.md`.
The planner keeps its rules: times follow the session's first tempo, the
tail is a whole number of half beats, and reverb plus echo returns are
power-summed against the dry track and lowered together when over `max_wet_db`.

## Rationale
A second implementation would duplicate a merged contract. A verification run
turns an unchecked claim into evidence at the cost of one test target.

## Consequences
- The sends are proposals only: aux buses hold no inserts yet (ADR-0045 5),
  so processor settings are carried in the plan, not applied. Applying them
  waits on Aux and Master inserts and is a follow-up.
- The planner takes reference numbers; it does not measure RT60 or pre-delay
  from audio. An analysis pass for that is a separate item.
- Unverified assumption: the snapping tables suit sessions within the tempo
  limits the planner enforces; a listening check on real material would settle
  it (hand to the researcher).

## Alternatives rejected
- Re-implement or extend the planner under this item: duplicates #437.
- Tick the item with no run: a check not run is unverified, not passing.
