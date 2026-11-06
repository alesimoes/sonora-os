# Planning

- No task starts without an approved plan in `planning/plans/<item-id>.md`
  (template: `planning/plans/TEMPLATE.md`, skill `plan-task`). Why: owner,
  2026-11-06; work started without plans produced half-features and fix rounds.
- The plan is broken down by role; all role agents then work in parallel on
  their sections, each on its own sub-branch.
- Approved = architect signed the contracts section, every section filled or
  N/A with a reason, a default for every open question.
- Contracts first, as their own small PR; slices start from it.
- An implementer whose task has no approved plan stops and reports; it does
  not plan for itself. A plan that proves wrong is amended and re-signed, not
  silently ignored.
