---
name: plan-task
description: Write the complete plan of a task before any development starts. Use when a BACKLOG item (a plugin, a screen, a flow) has no approved planning/plans/<item-id>.md.
---

# Plan the whole task, then split it by role

"No task leaves without a complete plan" (owner, 2026-11-06).

1. `tools/progress.sh get ID`, then grep the item in the BACKLOG (never read it
   whole). Check it is not already on main (`git log origin/main --oneline -S`).
2. Copy `planning/plans/TEMPLATE.md` to `planning/plans/<item-id>.md`.
3. Read the area files in `.claude/context/` for the paths and ADRs; read ADR
   Summaries only.
4. Fill every section. A section that does not apply says `N/A - <reason>`.
   Deliverables are files; acceptance is a command or a screenshot.
5. Sizing: one plan is one PR. If it needs more than about 4 slices or more
   than one context's gate, split the item and plan each part.
6. Slices to agents (agents from `.claude/routing.toml`):
   - contract (architect + owner engineer): its own PR, merged first.
   - design (visual-designer), core (rt-engineer), dsp/host
     (dsp-engineer/plugin-engineer), UI (ui-engineer), tests/QA (tester).
   - Each slice gets a sub-branch `<feature>/<role>`, disjoint files, and a
     start-from (the contract branch). Name one integrator.
7. Reserve numbers: ask the coordinator for the ADR, protocol and format
   numbers; write them in section 3.
8. List risks with a recommended default each.
9. Commit the plan and hand it to `architect`.

## Approved means

- architect signed the contracts section (and ADR number) in the header;
- every section filled or N/A with a reason;
- slices have disjoint files, sub-branches and a named integrator;
- every open question has a default.

The orchestrator launches no implementer for a plan that is not `Status:
approved`. Changing an approved plan's contract is an amendment, re-signed.
