# feature/token-economy-harness

- **Goal:** cut token spend in the agent cycle with handoffs between short
  sessions, a test-loop skill, and output discipline. Asked for by the owner
  on 2026-10-12.
- **State:** in review. Local only, not pushed.
- **Commits:** see `git log origin/main..feature/token-economy-harness`.

## Verify
    git diff --stat origin/main   # expected: docs and skills only, no code

## Done
- `handoff` skill — `.claude/skills/handoff/SKILL.md`
- `test-loop` skill with measured loop timings — `.claude/skills/test-loop/SKILL.md`
- Handoff format and template — `planning/handoffs/README.md`
- `CLAUDE.md` "Working in sessions" (10 lines; every session pays for it)
- `OPERATIONS.md`: handoffs between steps, and a "Spending context" table
  built from the write-usb task
- `code-reviewer` reads the handoff, not the transcript, and flags skill
  text the diff has made false; `integrator` deletes the handoff at merge
- `build-iso` skill: stale partition layout fixed (persistence is
  partition 3), and it points at the real test scripts

## Open
- Whoever spawns agents (the coordinator) has to adopt "fresh agent per
  round". No file can enforce that.
- The timings in `test-loop` come from one host; refresh them if CI starts
  running these tests.

## Do not redo
- Do not put procedures in `CLAUDE.md`: it loads in every session, and skills
  load only when used.

## Pointers
- `.claude/OPERATIONS.md` "Spending context"
