# feature/token-economy-harness

- **Goal:** cut token spend in the agent cycle: short sessions joined by
  handoffs, a SessionStart hook that tells a new session where things stand,
  a test-loop skill, output discipline. Asked for by the owner on 2026-10-12.
- **State:** pushed, PR open; needs `code-reviewer` + `integrator`.
- **Commits:** `git log origin/main..feature/token-economy-harness`.

## Verify
    bash tools/session-start.sh   # expected: about 75 lines, branch + handoffs + pending decision
    git diff --stat origin/main   # expected: docs, skills, settings, one script

## Done
- SessionStart hook — `.claude/settings.json` runs `tools/session-start.sh`
  on startup, resume, /clear and compaction. The script was run by hand;
  the hook has not yet been seen firing in a new Claude Code session.
- `handoff` and `test-loop` skills — `.claude/skills/`
- Handoff format; `main-session.md` for the coordinating session — `planning/handoffs/`
- `CLAUDE.md` "Working in sessions"; `OPERATIONS.md` "Spending context"
- `code-reviewer` reads the handoff, not the transcript; `integrator`
  deletes branch handoffs at merge
- `build-iso` skill: stale stick layout fixed

## Open
- Confirm the hook fires: open a new `claude` session in the main checkout
  after merge; its first context should start "## Where we left off".
- The coordinator must adopt "fresh agent per round"; nothing enforces it.

## Do not redo
- No procedures in `CLAUDE.md`: every session pays for it; skills load on use.

## Pointers
- `planning/handoffs/README.md`, `.claude/OPERATIONS.md` "Spending context"
