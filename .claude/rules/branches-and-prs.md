# Branches and PRs

- Fresh branch from `origin/main` for every PR, new name; never reuse a
  branch; delete the local one afterwards. Why: reused branches carried old
  commits and conflicts (owner, 2026-11-05).
- One PR per complete feature (contract, engine, UI, tests, docs together),
  grouped by context so one PR pays one context's gate. A PR may be partial
  only for hardware or an owner decision, and its title says so.
- The ADR travels with the code it decides. Contracts are a small first PR of
  their own, merged before what builds on them.
- Run `tools/pre-pr.sh` before pushing; push once, not a chain of fixes.
- Only the orchestrator merges, after its local CI gate passes. Nobody merges
  by hand, skips the gate or arms `tools/merge-when-green.sh`. Why: manual
  merges bypassed the gate and put red on main.
- An implementer never edits `planning/BACKLOG.md` or runs `progress.sh set`;
  the `[x]` line and follow-ups go in the PR body. Never tick a line whose
  text still names an open part.
- Commit at the end of every stage so a stopped agent loses nothing. Commit
  messages end with the attribution line given in the session.
- Check the BACKLOG item is not already on main before starting
  (`git log origin/main --oneline -S`).
