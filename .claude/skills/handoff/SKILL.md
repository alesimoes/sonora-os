---
name: handoff
description: Hand work to the next session or agent cheaply. Use when stopping, at hand-back, before a review round or rebase, when a session has grown long, and when starting work on a branch that already has a handoff.
---

# Hand off, and start clean

Context is the main cost of an agent run. Every turn re-reads everything
already in the session, so a session that carries three review rounds pays
for round one three times. The cure is short sessions joined by a written
handoff.

## When stopping

1. Write `planning/handoffs/<branch>.md` from the template in
   `planning/handoffs/README.md`. 40 lines at most.
2. Put anything durable in `brain/` (the `brain-write` skill) and
   `planning/STATUS.md`, not in the handoff. The handoff dies at merge.
3. Commit it with the work.
4. The final report to the caller is the handoff's content, or its path,
   and nothing more: what changed, the evidence, what is open. No restating
   files the caller can open.

In the main (coordinating) session, also overwrite
`planning/handoffs/main-session.md`: what is in flight, what landed, what
waits on the owner, and what is next.

## When starting

1. The SessionStart hook has already printed the handoffs (see
   `planning/handoffs/README.md`). A spawned agent that did not get that
   output reads `planning/handoffs/<branch>.md` itself. Do not read the
   previous transcript, and do not re-read files the handoff says are
   settled.
2. Run its **Verify** command before changing anything, so you know the
   starting state rather than trusting it.
3. Skip everything under **Do not redo**.

## For whoever spawns agents

- One agent per task and per review round. Spawn a fresh one with the
  handoff path rather than sending a long-running agent another round.
- The reviewer gets the diff range and the handoff, not the implementer's
  transcript.
- In an interactive session, `/clear` between unrelated tasks; `/compact`
  only when the current task truly needs its history.
