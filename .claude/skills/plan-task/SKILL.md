---
name: plan-task
description: Write the complete plan of a task before any development starts (architect), or dispatch an approved plan (lead). Use when a BACKLOG item (a plugin, a screen, a flow) has no approved planning/plans/<item-id>.md.
---

# Plan the whole task, then split it by role

"No task leaves without a complete plan" (owner, 2026-11-06). The architect
writes the plan with this skill; the lead then distributes it. There is no
separate planner agent (`architect` and `lead` cover it).

1. `tools/progress.sh get ID`, then grep the item in the BACKLOG (never read it
   whole). Check it is not already on main (`git log origin/main --oneline -S`).
2. Copy `planning/plans/TEMPLATE.md` to `planning/plans/<item-id>.md`.
3. Read the area files in `.claude/context/` for paths and ADRs; ADR
   Summaries only.
4. Fill every section. A section that does not apply says `N/A - <reason>`.
   Deliverables are files; acceptance is a command or a screenshot.
5. Sizing: one plan is one PR. More than about 4 slices, or more than one
   context's gate: split the item and plan each part. No XL slice.
6. Grade every slice (difficulty S/M/L, risk low/med/high) and read the tier
   from the table in `.claude/rules/agents-and-models.md`. Contracts, RT,
   storage, recovery, concurrency are high risk.
7. Slices to agents (from `.claude/routing.toml`): contract (architect + owner
   engineer, its own PR, merged first); design (visual-designer); core
   (rt-engineer); dsp/host (dsp-engineer, plugin-engineer); UI (ui-engineer);
   tests/QA (tester). Each gets a sub-branch `sub/<feature>/<task>`, disjoint
   files, a start-from (the contract branch). Name one integrator.
8. Numbers: ADR, protocol and format numbers come from the coordinator's
   reservation; write them in section 3.
9. Risks with a recommended default each. Commit the plan, set `Status:
   approved` and sign the header when 10 holds.
10. Lead: on `plan_ready`, turn the slices into the assignment table
    (engineer, tester, sub-branch, order, start_from, model tier). Ambiguity
    that appears later goes back to the architect, who updates the plan; the
    lead re-dispatches.

## Approved means

- the architect decided the contracts section (and ADR number) and signed;
- every section filled or N/A with a reason;
- slices have disjoint files, sub-branches, a grade and tier, a named integrator;
- every open question has a default.

The orchestrator launches no engineer for a plan that is not `Status:
approved`. Changing an approved plan's contract is an amendment, re-signed.
