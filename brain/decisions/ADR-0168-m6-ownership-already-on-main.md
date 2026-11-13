# ADR-0168 — ADR-0007 M6 (ownership where agents read it) is already on main

Status: accepted

## Context
ADR-0007 §9 M6 asks for the §1 ownership table in `.claude/OPERATIONS.md`, each
agent charter's "What you own" on the new paths, and the CLAUDE.md layout
table updated (owner's edit).

## Decision
Close the item by reference. `.claude/OPERATIONS.md` "Who owns which directory"
holds the table; the rt, ui, driver, dsp and plugin charters carry "What you
own" with the §1 paths. No code or charter change is made. The CLAUDE.md layout
table is left as is: it lists top-level areas, which are still correct, and
edits to CLAUDE.md are the owner's.

## Rationale
Redoing landed work risks conflicting edits to files every agent reads.

## Consequences
BACKLOG line is ticked from the PR body. Only the ADR and INDEX line change.

## Alternatives rejected
Adding `contracts/`, `common/` rows to CLAUDE.md: owner-owned file, no need
since OPERATIONS.md is what agents read.
