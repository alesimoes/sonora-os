# ADR-0038 — Mix-assistant plans: model operations, a diff preview, one compound undo

- Status: Accepted (recommended default, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Item: `mixassist.plans` (mix assistant V1, contract first)
- Relates to: ADR-0009 (compound operations, mix outside undo), ADR-0011
  (§4.4 the assistant, N5), ADR-0007 (module DAG)

## Summary

A **plan** is an ordered list of ordinary `common/model` `Member`s, each
with a title and a reason, plus notes on what it left alone. It lives in
`common/model/plan.h` and never touches the audio thread. `preview()` returns
a diff (tracks, buses, sends, inserts, fader and send values) and the exact
snapshot that accepting would leave. The operator accepts any subset;
`accept_plan()` applies the accepted steps as ONE `Compound` through
`History`, so it is one revision, one replication update and one undo step.
Steps may `needs` earlier steps: rejecting the bus drops its sends.
Value members that `History` does not undo (`SetMix`, `SetSendMix`,
`SetBusMix`) are restored by a receipt, so "one undo reverts it all" holds;
other non-undoable members are refused in a plan. Sources of plans implement
`PlanSource`. No wire change: `kProtocolVersion` stays.

## Context

ADR-0011 §4.4 says the assistant's output is session operations, shown as a
reviewable diff, accepted whole, in part or not at all, and undone in one
step. Three agents (workflow, reverb/delay, mastering suggestions) will
build planners; without one contract each would invent its own diff and its
own undo. ADR-0009 gave us `Compound`, but also a rule that bites here: mix
values (faders, send levels, insert parameters) are saved and **not undone**
(§3). A plan that trims a fader would be undone by `History::undo()` in its
structure only, and the fader would stay: a silent failure of the "one undo"
promise.

## Decision

1. **A plan is data, in `common/model`.** `Plan{label, base_revision, steps,
   notes}`; `PlanStep{Member op, title, reason, needs}`. It adds no operation
   type and no codec: it is built, previewed and applied on the control
   thread (the engine's, or a client's through the existing operation
   path). The audio thread sees only the plan that `apply` publishes, as for
   any edit.
2. **Ids are reserved up front.** The source allocates every id from the
   base's `next_id`, increasing in step order (ADR-0009 compound rules). Any
   subset of steps then still applies; gaps are harmless.
3. **Dependencies are explicit.** `needs` lists earlier steps. A step
   applies iff it is accepted and every needed step applies. A dependent of
   a rejected step is *dropped and reported* (`Resolved::dropped`), never
   revived: the operator's "no" wins.
4. **Preview is the apply, not a model of it.** `preview()` applies the
   resolved steps one by one to a copy for the per-step `DiffLine`s and
   applies the same `Compound`, with the same `MemberCheck`, for `result`.
   The diff comes from `diff_snapshots()` over states, not from reading the
   operations, so it cannot disagree with the model. A refusal names the
   step: `step 2 (Reverb send on Vox): ...`.
5. **Accept is one compound.** `accept_plan()` calls `History::apply` once,
   labelled `plan.label` ("Undo Reverb Plan"). A stale `base_revision` is
   refused (a plan reads current values; applying it to another state would
   be wrong). An empty selection succeeds and applies nothing.
6. **Undo covers values through a receipt.** `accept_plan()` reads the
   pre-plan value of every `SetMix`/`SetSendMix`/`SetBusMix` target that
   already existed and stores them as a restore compound. `undo_plan()` runs
   `History::undo()` (if the compound made a step) and then the restore, and
   is refused unless the plan is still the latest revision, so it never
   undoes something else. Values of things the plan itself created need no
   restore: they outlive the undone add, as ADR-0009 §3 says. A member that
   is neither undoable nor restorable (`AddSource`, `SetSettings`,
   `SetInsertParams`, ...) makes the plan malformed (`plan_revertible`).
7. **A planner is a `PlanSource`.** `propose(const Snapshot&, Plan&, error)`
   on a non-RT thread; it fails with a reason instead of returning an empty
   plan. ADR-0011's rule data and the analysis process sit behind it. The
   assistant uses our native processors only; third-party inserts become
   `PlanNote`s ("not adjusted").
8. **Classification (ADR-0007 §6).** Additive: one new header and source in
   `common/model`, no change to `operations.h`, `contracts/api` or the wire.

## Rationale

Reusing `Compound` and `History` costs nothing in the engine and gives
replication, the undo window and the budget for free. The alternative cost
is real: plan-specific undo would be a second undo stack to keep consistent.
Making the diff a function of two snapshots means every future operation
shows up in the preview without a case per operation (only the display
vocabulary in `diff_snapshots` grows).

## Consequences

- The UI (ui-engineer, later) renders `PlanPreview` and calls
  `accept_plan`; it sends the resulting `Compound` over the existing path.
  Whether the engine or the client runs `accept_plan` is left to that item;
  both use the same `History`.
- **Known gap: redo.** `History::redo()` redoes the structure of a plan but
  not its value members; the fader stays at the pre-plan value. Re-accept
  the plan instead. This disappears when mix values get undo (BACKLOG
  Track 2 item 6); `undo_plan` then reduces to `History::undo()`.
- The restore compound is not on the wire; a client that restarts loses the
  receipt, and then only `History::undo()` is available (structure only).
- `diff_snapshots` covers tracks, buses, sends, inserts and their mix
  values. Regions, automation and MIDI are not shown yet; a plan that edits
  them previews no lines for them until added.
- A plan is limited to `kMaxCompoundMembers` (256) steps.

## Alternatives rejected

- **Plan as a new operation type with its own codec.** Duplicates
  `Compound`, forces a protocol bump and gives the UI a second path.
- **Diff computed from the operations.** Cheap, but drifts: it would need a
  case per operation and would show what the operation says, not what the
  model did (clamps, refusals, sorting).
- **Auto-accept the dependencies of an accepted step.** Convenient, but
  overrides an explicit rejection of the bus; the operator would see a
  processor they declined.
- **Applying mix values outside the compound (a second apply).** Two
  revisions and two replication updates for one plan, and a window in which
  the replica shows half of it.
- **Wait for mix undo before shipping plans.** Blocks three downstream items
  on Track 2 item 6 for a gap the receipt covers.

## Unverified

- That `Compound` plus the restore compound is acceptable to the engine's
  `MemberCheck` for every value member (fader ranges): the check runs on the
  restore as on any edit, and was not exercised with the real engine hook.
- The cost of `preview()` on a 256-step plan against the 10 ms control
  budget: it applies the plan twice (step by step, then as a compound). A
  measurement with `measure_compound`-style data would settle it; hand to the
  researcher before a planner emits more than a few dozen steps.
