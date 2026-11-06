# Agents and models

Owner, 2026-11-06: the model is chosen per task by its grade, not per agent.
This supersedes "Sonnet everywhere, Opus only for the principal" (2026-11-03/05).
Agent files stay role-based (rt-engineer, ui-engineer...); the launcher passes
`--model` from the task's tier.

## Tiers

| Tier | Model | For |
|---|---|---|
| Junior | `haiku` | small, low-risk: docs, test additions, mechanical edits, small isolated changes |
| Pleno | `sonnet` | default for normal feature work, reviews, QA |
| Senior | `opus` | hard or high-risk: RT path, contracts, storage/recovery, concurrency, cross-module refactors |
| Principal | `fable` | last resort only: same red after the senior's rounds, critical blocks (`principal-engineer`) |

## Grade -> tier

The plan grades every slice: difficulty S (a few lines, one file), M (one
module), L (several files or modules), XL (split it first; a plan never
carries XL), and risk low / med / high.

| Difficulty \ Risk | low | med | high |
|---|---|---|---|
| S | junior | pleno | senior |
| M | pleno | pleno | senior |
| L | pleno | senior | senior |

- Risk is high by definition for: the real-time path, `contracts/`, storage
  and recovery (a take must not be lost), concurrency, cross-module refactors.
- Junior never touches the RT path or contracts; a junior slice that grows
  past S is re-graded and re-assigned, not finished.
- Reviews (`code-reviewer`, `rt-safety-auditor`) run at least at the author's
  tier, never below pleno; `tester` is pleno. A re-review checks only the fix diff.
- Fable only after a senior spent its rounds; one at a time. Why: cost and
  weekly quota (owner, 2026-11-03/05/06) - spend the strong models where a
  mistake costs most.

## Other rules

- Specialist agents only, picked by path from `.claude/routing.toml`. Never a
  general-purpose agent for real work.
- The author never reviews or tests its own diff.
- Communication goes through the orchestrator's hub (`mcp__harness__send` /
  `recv`; ADR-0047), not around it. The `lead` assigns; nobody starts a task
  that was not assigned (`.claude/rules/planning.md`).
- Only a blocking finding sends a PR back; the rest become BACKLOG lines for
  the track's follow-ups PR.
- No agent waits for CI, backgrounds a process, or ends a turn asking the
  owner; a real blocker goes in the handoff under "waits on the owner".
- Context is the cost: read an ADR's Summary, grep its heading; read your
  area's `.claude/context/` file instead of big docs.
- Same command failing twice for the same reason: stop and report the cause.
