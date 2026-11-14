# ADR-0215: Mix assistant workflow — the mix-bus chain waits on bus inserts

Status: Proposed (2026-11-14)

## Context
The mix planners (gain staging, bus structure, parallel compression, saturation,
reverb/delay) are on main (#564, #437) and `mix_planner` passes its suite. Five
of the planned steps are done: the planner emits real plan steps for them.

The mix-bus chain (a compressor or EQ on the Master or on an Aux) is not. Aux and
Master strips host no inserts yet (ADR-0045 §5), so `AddInsert` cannot target them.
Until then the chain is carried as data only: `ProcessorSetting` in
`engine/control/include/sonora/mix_planner.h` holds the LV2 URI and parameters,
ready to become an `AddInsert`. Nothing in the plan applies it. Adding bus inserts
is owned by `ui.mixer.io.rest`, which changes the wire and needs its own protocol
version.

## Decision
`mixassist.workflow` does not add bus inserts. It ships as a partial feature: the
planners keep the bus chain as data and emit no step for it until `ui.mixer.io.rest`
merges. After that merge, a follow-up turns each `ProcessorSetting` for an Aux or the
Master into an `AddInsert` step, and the planner tests assert the step. This PR is
titled "partial: waits on bus inserts".

## Rationale
Bus inserts touch the wire and the mixer. Doing them here would duplicate the owner
item and risk a protocol clash with `ui.mixer.io.rest`. Keeping the chain as data
means nothing is applied to the signal path by guesswork: the user sees the chain
missing rather than a wrong insert placed silently on a track.

## Consequences
- The bus chain stays a manual step for the user until `ui.mixer.io.rest` lands.
- No source change in this PR: the ADR and the INDEX line are the whole delivery.
- One follow-up is queued behind `ui.mixer.io.rest`: planners emit `AddInsert` steps
  for Aux and Master chains. It needs no new protocol version of its own.
- The plan (`planning/plans/mixassist.workflow.md`) is re-planned when
  `ui.mixer.io.rest` merges, not before.

## Alternatives rejected
- Add Aux and Master inserts here: duplicates `ui.mixer.io.rest` and touches the wire.
- Emit `AddInsert` on a track as a stand-in for the bus chain: changes the signal path
  silently and places the processor on the wrong level of the mix.
- Hold the whole feature until the bus inserts land: the finished planners would
  wait for nothing they depend on.
