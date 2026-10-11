---
name: brain-write
description: Record durable knowledge in brain/ — an ADR, a research finding, a format spec, or a postmortem. Use at the end of any task that established something a future session would otherwise rediscover.
---

# Write to the brain

Session context is lost at the end of the run. `brain/` is what survives.

## Choose the right place

| You established | Goes in | Shape |
|---|---|---|
| A decision about architecture | `brain/decisions/ADR-NNNN-<slug>.md` | Context, Decision, Rationale, Consequences, Alternatives rejected |
| A fact about a format or protocol | `brain/specs/<name>.md` | Offsets, sizes, byte order, where real files disagree with the standard |
| A measurement or an investigation | `brain/research/<topic>.md` | What you ran, on what hardware, what it produced |
| Something that broke | `brain/postmortems/<date>-<slug>.md` | Symptom, root cause, the regression test that now guards it |

Then add one line to `brain/INDEX.md`. An entry that is not in the index will
not be found.

## Rules

One subject per file. If the title needs "and", write two files.

Cite sources. A URL or a document section for anything from outside; the
machine and the command for anything measured here.

Mark the unverified as unverified. "The specification allows 32 channels" and
"we recorded 32 channels at 64 frames on this interface" are different claims
and must never be written as if they were the same one.

Write down open questions. They are the most useful thing in the file for
whoever arrives next.

ADRs are append-only once accepted. To reverse one, write a new ADR and set
`Superseded by:` in the header of the old one.

This repository is public. No secrets, no customer data, no scratch notes.

## Also update the plan

If the work changed what is true about the project state, update
`planning/STATUS.md`. If it changed what we intend to do, update
`planning/BACKLOG.md`. Stale planning files are worse than absent ones.
