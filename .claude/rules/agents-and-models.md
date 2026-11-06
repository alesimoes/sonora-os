# Agents and models

- Every agent and every review runs on Sonnet (`model: sonnet`). Only
  `principal-engineer` runs on Opus, for escalations only. Why: Opus burned
  the weekly quota (owner, 2026-11-03/05). Hard areas (RT, contracts,
  storage, recovery) get a more careful brief, not a bigger model.
- Specialist agents only. Never a general-purpose agent for real work; pick
  by path from `.claude/routing.toml`. Why: generic agents did specialist
  work badly and cost review rounds.
- The author never reviews or tests its own diff (`code-reviewer`, `tester`).
- Only a blocking finding sends a PR back; the rest become BACKLOG lines for
  the track's follow-ups PR. A re-review is a fresh agent on the fix diff only.
- No agent waits for CI, backgrounds a process, or ends a turn asking the
  owner; a real blocker goes in the handoff under "waits on the owner".
- Context is the cost: read an ADR's Summary, grep its heading; read your
  area's `.claude/context/` file instead of big docs.
- Same command failing twice for the same reason: stop and report the cause.
- Staffing and ADR-number reservation belong to the coordinator.
