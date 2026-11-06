# Planning and dispatch

- No task starts without an approved plan in `planning/plans/<item-id>.md`
  (template: `planning/plans/TEMPLATE.md`, skill `plan-task`). Why: owner,
  2026-11-06; work started without plans produced half-features and fix rounds.
- The `architect` writes the plan; the `lead` distributes it. No engineer
  starts until the lead assigns the slice (who does what, from the approved
  plan). An engineer without an assignment stops and reports.
- The plan is broken down by role; the role agents then work in parallel on
  their sections, each on its own sub-branch.
- Every slice carries a grade (difficulty, risk) and the tier it picks
  (`.claude/rules/agents-and-models.md`).
- Approved = contracts and ADR number decided by the architect, every
  section filled or N/A with a reason, a default for every open question.
- Missing definition or ambiguity found by anyone: message the architect
  (`mcp__harness__send`), who updates the plan; the lead re-dispatches. Do not
  guess and do not plan for yourself.
- A bug found by QA: the lead assigns an engineer by area (`routing.toml`) to
  fix it; QA re-tests.
- Contracts first, as their own small PR; slices start from it.
